/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2024  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "lobby.h"
#include "application.h"
#include "constants.h"
#include "glm/geometric.hpp"
#include "hand_model.h"
#include "hardware.h"
#include "imgui.h"
#include "openxr/openxr.h"
#include "protocol_version.h"
#include "render/scene_data.h"
#include "stream.h"
#include "utils/i18n.h"
#include "wivrn_client.h"
#include "wivrn_discover.h"
#include "wivrn_sockets.h"
#include "xr/passthrough.h"
#include "xr/space.h"
#include <glm/gtc/matrix_access.hpp>

#include <chrono> // IWYU pragma: keep
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <magic_enum.hpp>
#include <ranges>
#include <simdjson.h>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/poll.h>
#include <vulkan/vulkan_raii.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

using namespace std::chrono_literals;

static bool force_autoconnect = false;

scenes::lobby::~lobby()
{
	if (renderer)
		renderer->wait_idle();
}

static const std::array supported_color_formats = {
        vk::Format::eR8G8B8A8Srgb,
        vk::Format::eB8G8R8A8Srgb,
};

static const std::array supported_depth_formats{
        vk::Format::eD32Sfloat,
        vk::Format::eX8D24UnormPack32,
};

static glm::quat compute_gui_orientation(glm::vec3 head_position, glm::vec3 new_gui_position)
{
	using constants::lobby::gui_pitch;

	glm::vec3 gui_direction = new_gui_position - head_position;

	float gui_yaw = atan2(gui_direction.x, gui_direction.z) + M_PI;

	return glm::quat(cos(gui_yaw / 2), 0, sin(gui_yaw / 2), 0) * glm::quat(cos(gui_pitch / 2), sin(gui_pitch / 2), 0, 0);
}

void scenes::lobby::move_gui(glm::vec3 head_position, glm::vec3 new_gui_position)
{
	using constants::lobby::keyboard_pitch;
	using constants::lobby::keyboard_position;
	using constants::lobby::popup_position;

	auto q = compute_gui_orientation(head_position, new_gui_position);
	auto M = glm::mat3_cast(q); // plane-to-world transform

	// Main window
	imgui_ctx->layers()[0].position = new_gui_position;
	imgui_ctx->layers()[0].orientation = q;

	// Popup
	imgui_ctx->layers()[1].position = new_gui_position + M * popup_position;
	imgui_ctx->layers()[1].orientation = q;

	// Keyboard
	imgui_ctx->layers()[2].position = new_gui_position + M * keyboard_position;
	imgui_ctx->layers()[2].orientation = q * glm::quat(cos(keyboard_pitch / 2), sin(keyboard_pitch / 2), 0, 0);
}

scenes::lobby::lobby() :
        scene_impl<lobby>(supported_color_formats, supported_depth_formats)
{
	spdlog::info("Using formats {} and {}", vk::to_string(swapchain_format), vk::to_string(depth_format));
	// composition_layer_depth_test_supported = false;
	if (composition_layer_depth_test_supported)
		spdlog::info("Composition layer depth test supported");
	else
		spdlog::info("Composition layer depth test NOT supported");

	if (composition_layer_color_scale_bias_supported)
		spdlog::info("Composition layer color scale/bias supported");
	else
		spdlog::info("Composition layer color scale/bias NOT supported");

	if (std::getenv("WIVRN_AUTOCONNECT"))
		force_autoconnect = true;

	auto & config = application::get_config();

	auto & servers = config.servers;
	spdlog::info("{} known server(s):", servers.size());
	for (auto & i: servers)
	{
		spdlog::info("    {}", i.second.service.name);
	}

	keyboard.set_layout(config.virtual_keyboard_layout);

	if (config.first_run)
		current_tab = tab::first_run;

	const auto keypair_path = application::get_config_path() / "private_key.pem";
	try
	{
		std::ifstream f{keypair_path};
		keypair = crypto::key::from_private_key(std::string{(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()});
	}
	catch (...)
	{
		keypair = crypto::key::generate_x448_keypair();
		std::ofstream{keypair_path} << keypair.private_key();
		spdlog::info("Generated X448 keypair");
	}
}

static std::string ip_address_to_string(const in_addr & addr)
{
	char buf[100];
	inet_ntop(AF_INET, &addr, buf, sizeof(buf));
	return buf;
}

static std::string ip_address_to_string(const in6_addr & addr)
{
	char buf[100];
	inet_ntop(AF_INET6, &addr, buf, sizeof(buf));
	return buf;
}

std::unique_ptr<wivrn_session> scenes::lobby::connect_to_session(wivrn_discover::service service, bool manual_connection)
{
	if (!manual_connection)
	{
		char protocol_string[17];
		sprintf(protocol_string, "%016lx", wivrn::protocol_version);

		spdlog::debug("Client protocol version: {}", protocol_string);
		spdlog::debug("Server TXT:");
		for (auto & [i, j]: service.txt)
		{
			spdlog::debug("    {}=\"{}\"", i, j);
		}

		auto protocol = service.txt.find("protocol");
		if (protocol == service.txt.end())
			throw std::runtime_error(_("Incompatible WiVRn server: no protocol field in TXT"));

		if (protocol->second != protocol_string)
			throw std::runtime_error(fmt::format(_F("Incompatible WiVRn server protocol (client: {}, server: {})"), protocol_string, protocol->second));
	}

	// Only the automatically discovered servers already have their IP addresses available
	if (manual_connection)
	{
		addrinfo hint{
		        .ai_flags = AI_ADDRCONFIG,
		        .ai_family = AF_UNSPEC,
		        .ai_socktype = SOCK_STREAM,
		};
		addrinfo * addresses;
		if (int err = getaddrinfo(service.hostname.c_str(), nullptr, &hint, &addresses))
		{
			spdlog::error("Cannot resolve hostname {}: {}", service.hostname, gai_strerror(err));
			throw std::runtime_error(fmt::format(_F("Cannot resolve hostname: {}"), _(gai_strerror(err))));
		}

		for (auto i = addresses; i; i = i->ai_next)
		{
			switch (i->ai_family)
			{
				case AF_INET:
					service.addresses.push_back(((sockaddr_in *)i->ai_addr)->sin_addr);
					break;
				case AF_INET6:
					service.addresses.push_back(((sockaddr_in6 *)i->ai_addr)->sin6_addr);
					break;
			}
		}

		freeaddrinfo(addresses);
	}

	std::string error;
	for (const std::variant<in_addr, in6_addr> & address: service.addresses)
	{
		std::string address_string = std::visit([](auto & address) {
			return ip_address_to_string(address);
		},
		                                        address);

		struct connection_cancelled
		{};

		try
		{
			spdlog::debug("Connection to {}", address_string);

			return std::visit([this, &service](auto & address) {
				return std::make_unique<wivrn_session>(address, service.port, service.tcp_only, keypair, [&](int fd) {
					auto request = pin_request.lock();
					request->pin_requested = true;
					request->pin_cancelled = false;
					request->pin = service.pin;
					pin_buffer = "";

					while (not request.wait_for(500ms, [&]() { return request->pin != "" or request->pin_cancelled; }))
					{
						pollfd fds{};
						fds.events = POLLRDHUP;
						fds.fd = fd;

						int r = ::poll(&fds, 1, 0);

						if (r < 0)
						{
							request->pin_requested = false;
							throw std::system_error(errno, std::system_category());
						}

						if (fds.revents & (POLLHUP | POLLERR))
						{
							request->pin_requested = false;
							throw std::runtime_error("Error on control socket");
						}

						if (fds.revents & POLLRDHUP)
						{
							request->pin_requested = false;
							throw socket_shutdown{};
						}
					}

					request->pin_requested = false;

					if (request->pin_cancelled)
						throw connection_cancelled{};

					return request->pin;
				});
			},
			                  address);
		}
		catch (connection_cancelled)
		{
			spdlog::info("Connection cancelled");
			return nullptr;
		}
		catch (handshake_error & e)
		{
			spdlog::warn("Error during handshake to {} ({}): {}", service.hostname, address_string, e.what());
			std::string txt = fmt::format(_F("Cannot connect to {} ({}): {}"), service.hostname, address_string, e.what());
			throw std::runtime_error(txt);
		}
		catch (std::exception & e)
		{
			spdlog::warn("Cannot connect to {} ({}): {}", service.hostname, address_string, e.what());
			std::string txt = fmt::format(_F("Cannot connect to {} ({}): {}"), service.hostname, address_string, e.what());
			if (not error.empty())
				error += "\n";
			error += txt;
		}
	}

	throw std::runtime_error(error);
}

void scenes::lobby::update_server_list()
{
	if (application::is_focused() && !discover)
		discover.emplace();
	else if (!application::is_focused() && discover)
		discover.reset();

	if (!discover)
		return;

	std::vector<wivrn_discover::service> discovered_services = discover->get_services();

	// TODO: only if discovered_services changed
	auto & servers = application::get_config().servers;
	for (auto & [cookie, data]: servers)
	{
		data.visible = false;
	}

	char protocol_string[17];
	sprintf(protocol_string, "%016lx", wivrn::protocol_version);

	for (auto & service: discovered_services)
	{
		std::string cookie;
		bool compatible = true;

		auto it = service.txt.find("cookie");
		if (it == service.txt.end())
		{
			cookie = service.hostname;
			compatible = false;
		}
		else
			cookie = it->second;

		auto protocol = service.txt.find("protocol");
		if (protocol == service.txt.end())
			compatible = false;

		if (protocol->second != protocol_string)
			compatible = false;

		auto server = servers.find(cookie);
		if (server == servers.end())
		{
			// Newly discovered server: add it to the list
			servers.emplace(
			        cookie,
			        configuration::server_data{
			                .autoconnect = false,
			                .manual = false,
			                .visible = true,
			                .compatible = compatible,
			                .service = service,
			        });
		}
		else
		{
			server->second.visible = true;
			server->second.service = service;
			server->second.compatible = compatible;
		}
	}
}

void scenes::lobby::connect(const configuration::server_data & data)
{
	server_name = data.service.name;
	async_error.reset();

	async_session = utils::async<std::unique_ptr<wivrn_session>, std::string>(
	        [this](auto token, wivrn_discover::service service, bool manual) {
		        token.set_progress(_("Waiting for connection"));
		        return connect_to_session(service, manual);
	        },
	        data.service,
	        data.manual);
}

std::optional<glm::vec3> scenes::lobby::check_recenter_gesture(const std::array<xr::hand_tracker::joint, XR_HAND_JOINT_COUNT_EXT> & joints)
{
	const auto & palm = joints[XR_HAND_JOINT_PALM_EXT].first;
	const auto & o = palm.pose.orientation;
	const auto & p = palm.pose.position;
	glm::quat q{o.w, o.x, o.y, o.z};
	glm::vec3 v{p.x, p.y, p.z};

	if (glm::dot(q * glm::vec3(0, 1, 0), glm::vec3(0, -1, 0)) > constants::lobby::recenter_cosangle_min)
	{
		return v + glm::vec3(0, constants::lobby::recenter_distance_up, 0) + q * glm::vec3(0, 0, -constants::lobby::recenter_distance_front);
	}

	return std::nullopt;
}

std::optional<glm::vec3> scenes::lobby::check_recenter_action(XrTime predicted_display_time, glm::vec3 head_position)
{
	std::optional<std::pair<glm::vec3, glm::quat>> aim;
	glm::vec3 offset_position;
	glm::quat offset_orientation;

	if (application::read_action_bool(recenter_left_action).value_or(std::pair{0, false}).second)
	{
		aim = application::locate_controller(application::space(xr::spaces::aim_left), application::space(xr::spaces::world), predicted_display_time);
		std::tie(offset_position, offset_orientation) = input->offset[xr::spaces::aim_left];
	}

	if (application::read_action_bool(recenter_right_action).value_or(std::pair{0, false}).second)
	{
		aim = application::locate_controller(application::space(xr::spaces::aim_right), application::space(xr::spaces::world), predicted_display_time);
		std::tie(offset_position, offset_orientation) = input->offset[xr::spaces::aim_right];
	}

	if (aim)
	{
		// Handle controller offset
		aim->first = aim->first + glm::mat3_cast(aim->second * offset_orientation) * offset_position;
		aim->second = aim->second * offset_orientation;

		if (not gui_recenter_position or not gui_recenter_distance)
		{
			// First frame where the recenter action is active
			imgui_context::controller_state state{
			        .active = true,
			        .aim_position = aim->first,
			        .aim_orientation = aim->second,
			};

			imgui_ctx->compute_pointer_position(state);

			if (state.pointer_position) // TODO: check that the pointer is inside an imgui window
			{
				auto M = glm::mat3_cast(imgui_ctx->layers()[0].orientation);

				// Pointer position in world
				glm::vec3 world_pointer_position = imgui_ctx->rw_from_vp(*state.pointer_position);

				// Pointer position in GUI
				gui_recenter_position = glm::transpose(M) * (world_pointer_position - imgui_ctx->layers()[0].position);
				gui_recenter_distance = glm::length(state.aim_position - world_pointer_position);
			}
			else
			{
				gui_recenter_position = glm::vec3(0, 0, 0);
				gui_recenter_distance = constants::lobby::recenter_action_distance;
			}
		}
		else
		{
			// Subsequent frames: find the GUI position that gives the correct world pointer position
			glm::vec3 controller_direction = -glm::column(glm::mat3_cast(aim->second), 2);
			glm::vec3 wanted_world_pointer_position = aim->first + controller_direction * *gui_recenter_distance;

			// I'm sure there's an analytical solution but I can't be bothered to write it so
			// let's use a gradient descent instead
			auto f = [&](glm::vec3 new_gui_position) {
				auto q = compute_gui_orientation(head_position, new_gui_position);
				auto M = glm::mat3_cast(q); // plane-to-world transform

				glm::vec3 world_pointer_position = new_gui_position + M * *gui_recenter_position;

				return world_pointer_position - wanted_world_pointer_position;
			};

			// One step is usually enough, the solution will continuously improve in the next frames
			glm::vec3 gui_position = imgui_ctx->layers()[0].position;
			float eps = 0.01;
			glm::vec3 obj = f(gui_position);
			glm::vec3 obj_dx = (f(gui_position + glm::vec3(eps, 0, 0)) - obj) / eps;
			glm::vec3 obj_dy = (f(gui_position + glm::vec3(0, eps, 0)) - obj) / eps;
			glm::vec3 obj_dz = (f(gui_position + glm::vec3(0, 0, eps)) - obj) / eps;

			glm::mat3 jacobian{obj_dx, obj_dy, obj_dz};

			gui_position -= glm::inverse(jacobian) * obj;

			return gui_position;
		}
	}
	else
	{
		gui_recenter_position.reset();
	}

	return std::nullopt;
}

std::optional<glm::vec3> scenes::lobby::check_recenter_gui(glm::vec3 head_position, glm::quat head_orientation)
{
	glm::vec3 head_direction = -glm::column(glm::mat3_cast(head_orientation), 2);

	if (recenter_gui)
	{
		recenter_gui = false;
		glm::vec3 new_gui_position = head_position + constants::lobby::initial_gui_distance * head_direction;
		new_gui_position.y = head_position.y - 0.1;
		return new_gui_position;
	}

	return std::nullopt;
}

// Return the vector v such that dot(v, x) > 0 iff x is on the side where the composition layer is visible
static glm::vec4 compute_ray_limits(const XrPosef & pose, float margin = 0)
{
	glm::quat q{
	        pose.orientation.w,
	        pose.orientation.x,
	        pose.orientation.y,
	        pose.orientation.z,
	};

	glm::vec3 p{
	        pose.position.x,
	        pose.position.y,
	        pose.position.z,
	};

	glm::vec3 normal = glm::column(glm::mat3_cast(q), 2);

	return glm::vec4(normal, -glm::dot(p, normal) - margin);
}

void scenes::lobby::render(const XrFrameState & frame_state)
{
	if (async_session.valid() && async_session.poll() == utils::future_status::ready)
	{
		try
		{
			auto session = async_session.get();
			if (session)
				next_scene = stream::create(std::move(session), 1'000'000'000.f / frame_state.predictedDisplayPeriod);

			async_session.reset();
		}
		catch (std::exception & e)
		{
			spdlog::error("Error connecting to server: {}", e.what());
			async_session.cancel();
			async_error = e.what();
		}
	}

	if (next_scene)
	{
		if (!next_scene->alive())
			next_scene.reset();
		else if (next_scene->current_state() == scenes::stream::state::streaming)
		{
			autoconnect_enabled = true;
			application::push_scene(next_scene);
		}
	}

	update_server_list();

	imgui_ctx->set_current();
	if (!async_session.valid() && !next_scene && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))
	{
		if (auto intent = application::get_intent())
		{
			pin_request.lock()->pin = intent->pin;
			connect(configuration::server_data{
			        .manual = true,
			        .service = *intent,
			});
		}
		else
		{
			const auto & servers = application::get_config().servers;
			for (auto && [cookie, data]: servers)
			{
				if (data.visible && (data.autoconnect || force_autoconnect) && data.compatible && autoconnect_enabled)
				{
					autoconnect_enabled = false;
					connect(data);
					break;
				}
			}
		}
	}

	if (not frame_state.shouldRender)
	{
		session.begin_frame();
		session.end_frame(frame_state.predictedDisplayTime, {});
		return;
	}

	session.begin_frame();

	XrSpace world_space = application::space(xr::spaces::world);
	auto [flags, views] = session.locate_views(viewconfig, frame_state.predictedDisplayTime, world_space);
	// assert(views.size() == swapchains_lobby.size());
	assert(views.size() == 2); // FIXME

	bool hide_left_controller = false;
	bool hide_right_controller = false;

	std::optional<std::pair<glm::vec3, glm::quat>> head_position = application::locate_controller(application::space(xr::spaces::view), world_space, frame_state.predictedDisplayTime);
	std::optional<glm::vec3> new_gui_position;

	if (head_position)
		new_gui_position = check_recenter_gui(head_position->first, head_position->second);

	if (!new_gui_position and head_position)
		new_gui_position = check_recenter_action(frame_state.predictedDisplayTime, head_position->first);

	if (application::get_hand_tracking_supported())
	{
		auto left = application::get_left_hand().locate(world_space, frame_state.predictedDisplayTime);
		auto right = application::get_right_hand().locate(world_space, frame_state.predictedDisplayTime);

		hand_model::apply(world, left, right);

		if (left and xr::hand_tracker::check_flags(*left, XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT, 0))
		{
			hide_left_controller = true;
			if (!new_gui_position)
				new_gui_position = check_recenter_gesture(*left);
		}

		if (right and xr::hand_tracker::check_flags(*right, XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT, 0))
		{
			hide_right_controller = true;
			if (!new_gui_position)
				new_gui_position = check_recenter_gesture(*right);
		}
	}

	if (head_position && new_gui_position)
	{
		move_gui(head_position->first, *new_gui_position);
	}

#if WIVRN_CLIENT_DEBUG_MENU
	{
		auto & left_node = world.get<components::node>(xyz_axes_left_controller);
		auto & right_node = world.get<components::node>(xyz_axes_right_controller);
		if (display_debug_axes)
		{
			const xr::spaces left = display_grip_instead_of_aim ? xr::spaces::grip_left : xr::spaces::aim_left;

			if (hide_left_controller)
				left_node.visible = false;
			else if (auto location = application::locate_controller(application::space(left), world_space, frame_state.predictedDisplayTime))
			{
				left_node.visible = true;
				left_node.position = location->first;
				left_node.orientation = location->second;
			}
			else
				left_node.visible = false;

			const xr::spaces right = display_grip_instead_of_aim ? xr::spaces::grip_right : xr::spaces::aim_right;
			if (hide_right_controller)
				right_node.visible = false;
			else if (auto location = application::locate_controller(application::space(right), world_space, frame_state.predictedDisplayTime))
			{
				right_node.visible = true;
				right_node.position = location->first;
				right_node.orientation = location->second;
			}
			else
				right_node.visible = false;
		}
		else
		{
			left_node.visible = false;
			right_node.visible = false;
		}
	}
#endif

	std::vector<std::pair<int, XrCompositionLayerQuad>> imgui_layers = draw_gui(frame_state.predictedDisplayTime);

	// Get the planes that limit the ray size from the composition layers
	std::vector<glm::vec4> ray_limits;
	for (auto & [z_index, layer]: imgui_layers)
	{
		if (z_index != constants::lobby::zindex_recenter_tip)
			ray_limits.push_back(compute_ray_limits(layer.pose));
	}

	input->apply(world, world_space, frame_state.predictedDisplayTime, hide_left_controller, hide_right_controller, ray_limits);

	assert(renderer);

	world.get<components::node>(lobby_entity).visible = not application::get_config().passthrough_enabled;

	render_start(application::get_config().passthrough_enabled, frame_state.predictedDisplayTime);

	const XrColor4f clear_color = application::get_config().passthrough_enabled ? XrColor4f{0, 0, 0, 0} : constants::lobby::sky_color;
	render_world(
	        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
	        world_space,
	        views,
	        width,
	        height,
	        composition_layer_depth_test_supported,
	        composition_layer_depth_test_supported ? layer_lobby | layer_controllers : layer_lobby,
	        clear_color);

	if (composition_layer_depth_test_supported)
		set_depth_test(true, XR_COMPARE_OP_ALWAYS_FB);

	bool dim_gui = imgui_ctx->is_modal_popup_shown() and composition_layer_color_scale_bias_supported;
	for (auto & [z_index, layer]: imgui_layers)
	{
		if (z_index < constants::lobby::zindex_recenter_tip)
		{
			add_quad_layer(layer.layerFlags, layer.space, layer.eyeVisibility, layer.subImage, layer.pose, layer.size);

			if (dim_gui)
				set_color_scale_bias(constants::lobby::dimming_scale, constants::lobby::dimming_bias);

			if (composition_layer_depth_test_supported)
				set_depth_test(true, XR_COMPARE_OP_LESS_OR_EQUAL_FB);

			dim_gui = false; // Only dim the main window
		}
	}

	render_world(
	        XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
	        world_space,
	        views,
	        width,
	        height,
	        composition_layer_depth_test_supported,
	        composition_layer_depth_test_supported ? layer_rays : layer_rays | layer_controllers,
	        {0, 0, 0, 0});

	if (composition_layer_depth_test_supported)
		set_depth_test(true, XR_COMPARE_OP_LESS_OR_EQUAL_FB);

	for (auto & [z_index, layer]: imgui_layers)
	{
		if (z_index == constants::lobby::zindex_recenter_tip)
			add_quad_layer(layer.layerFlags, layer.space, layer.eyeVisibility, layer.subImage, layer.pose, layer.size);
	}

	render_end();
}

void scenes::lobby::on_focused()
{
	recenter_gui = true;

	auto views = system.view_configuration_views(viewconfig);
	assert(views.size() == 2); // FIXME
	stream_view = override_view(views[0], guess_model());
	width = views[0].recommendedImageRectWidth;
	height = views[0].recommendedImageRectHeight;

	// assert(std::ranges::all_of(views, [width](const XrViewConfigurationView & view) { return view.recommendedImageRectWidth == width; }));
	// assert(std::ranges::all_of(views, [height](const XrViewConfigurationView & view) { return view.recommendedImageRectHeight == height; }));

	renderer.emplace(device, physical_device, queue, commandpool);
	loader.emplace(device, physical_device, queue, application::queue_family_index(), renderer->get_default_material());

	lobby_entity = load_gltf("ground.gltf", layer_lobby).first;

	std::string profile = controller_name();
	input.emplace(
	        *this,
	        "controllers/" + profile + "/profile.json",
	        layer_controllers,
	        layer_rays);

	spdlog::info("Loaded input profile {}", input->id);

	for (auto i: {xr::spaces::aim_left, xr::spaces::aim_right, xr::spaces::grip_left, xr::spaces::grip_right})
	{
		auto [p, q] = input->offset[i] = controller_offset(controller_name(), i);

		auto rot = glm::degrees(glm::eulerAngles(q));
		spdlog::info("Initializing offset of space {} to ({}, {}, {}) mm, ({}, {}, {})°",
		             magic_enum::enum_name(i),
		             1000 * p.x,
		             1000 * p.y,
		             1000 * p.z,
		             rot.x,
		             rot.y,
		             rot.z);
	}

#if WIVRN_CLIENT_DEBUG_MENU
	offset_position = input->offset[xr::spaces::grip_left].first;
	offset_orientation = glm::degrees(glm::eulerAngles(input->offset[xr::spaces::grip_left].second));
	ray_offset = input->offset[xr::spaces::aim_left].first.z;

	xyz_axes_left_controller = load_gltf("xyz-arrows.glb", layer_controllers).first;
	xyz_axes_right_controller = load_gltf("xyz-arrows.glb", layer_controllers).first;
#endif

	if (application::get_hand_tracking_supported())
	{
		hand_model::add_hand(*this, XR_HAND_LEFT_EXT, "left-hand.glb", layer_controllers);
		hand_model::add_hand(*this, XR_HAND_RIGHT_EXT, "right-hand.glb", layer_controllers);
	}

	recenter_left_action = get_action("recenter_left").first;
	recenter_right_action = get_action("recenter_right").first;

	std::vector imgui_inputs{
	        imgui_context::controller{
	                .aim = get_action_space("left_aim"),
	                .offset = input->offset[xr::spaces::aim_left],
	                .trigger = get_action("left_trigger").first,
	                .squeeze = get_action("left_squeeze").first,
	                .scroll = get_action("left_scroll").first,
	                .haptic_output = get_action("left_haptic").first,
	        },
	        imgui_context::controller{
	                .aim = get_action_space("right_aim"),
	                .offset = input->offset[xr::spaces::aim_right],
	                .trigger = get_action("right_trigger").first,
	                .squeeze = get_action("right_squeeze").first,
	                .scroll = get_action("right_scroll").first,
	                .haptic_output = get_action("right_haptic").first,
	        },
	};
	if (auto & hand = application::get_left_hand())
	{
		imgui_inputs.push_back(
		        {
		                .hand = &hand,
		        });
	}
	if (auto & hand = application::get_right_hand())
	{
		imgui_inputs.push_back(
		        {
		                .hand = &hand,
		        });
	}

	// 0.4mm / pixel
	std::vector<imgui_context::viewport> vps{
	        {
	                // Main window
	                .space = xr::spaces::world,
	                .size = {0.6, 0.4},
	                .vp_origin = {0, 0},
	                .vp_size = {1500, 1000},
	                .z_index = constants::lobby::zindex_gui,
	        },
	        {
	                // Pop up window
	                .space = xr::spaces::world,
	                .size = {0.6, 0.28},
	                .vp_origin = {1500, 0},
	                .vp_size = {1500, 700},
	                .z_index = constants::lobby::zindex_gui,
	        },
	        {
	                // Virtual keyboard
	                .space = xr::spaces::world,
	                .size = {0.6, 0.2},
	                .vp_origin = {1500, 700},
	                .vp_size = {1500, 500},
	                .always_show_cursor = true,
	                .z_index = constants::lobby::zindex_gui,
	        },
	        {
	                // Recenter tip
	                .space = xr::spaces::view,
	                .position = {0, -0.4, -1.0},
	                .orientation = {1, 0, 0, 0},
	                .size = {0.6, 0.12},
	                .vp_origin = {0, 1000},
	                .vp_size = {1500, 300},
	                .z_index = constants::lobby::zindex_recenter_tip,
	        }};

	swapchain_imgui = xr::swapchain(session, device, swapchain_format, 3000, 1300);

	imgui_ctx.emplace(physical_device, device, queue_family_index, queue, imgui_inputs, swapchain_imgui, vps);

	auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	auto tm = std::localtime(&t);
	std::string image = tm->tm_mon == 5 ? "wivrn-pride" : "wivrn";
	try
	{
		about_picture = imgui_ctx->load_texture(image + ".ktx2");
	}
	catch (...)
	{
		about_picture = imgui_ctx->load_texture(image + ".png");
	}
	setup_passthrough();
	session.set_refresh_rate(application::get_config().preferred_refresh_rate.value_or(0));
	multicast = application::get_wifi_lock().get_multicast_lock();
}

void scenes::lobby::setup_passthrough()
{
	if (application::get_config().passthrough_enabled)
		session.enable_passthrough(system);
	else
		session.disable_passthrough();
}

void scenes::lobby::on_unfocused()
{
	discover.reset();

	renderer->wait_idle(); // Must be before the scene data because the renderer uses its descriptor sets

	about_picture = 0;
	imgui_ctx.reset();
	world.clear(); // Must be cleared before the renderer so that the descriptor sets are freed before their pools

	input.reset();

	loader.reset();
	renderer.reset();
	clear_swapchains();
	swapchain_imgui = xr::swapchain();
	multicast.reset();
}

void scenes::lobby::on_xr_event(const xr::event & event)
{
	switch (event.header.type)
	{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			if (event.state_changed.state == XR_SESSION_STATE_STOPPING)
				discover.reset();
			recenter_gui = true;
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			recenter_gui = true;
			break;
		default:
			break;
	}
	if (next_scene)
		next_scene->on_xr_event(event);
}

scene::meta & scenes::lobby::get_meta_scene()
{
	static meta m{
	        .name = "Lobby",
	        .actions = {
	                {"left_aim", XR_ACTION_TYPE_POSE_INPUT},
	                {"left_trigger", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"left_squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"left_scroll", XR_ACTION_TYPE_VECTOR2F_INPUT},
	                {"left_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
	                {"right_aim", XR_ACTION_TYPE_POSE_INPUT},
	                {"right_trigger", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"right_squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
	                {"right_scroll", XR_ACTION_TYPE_VECTOR2F_INPUT},
	                {"right_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},

	                {"recenter_left", XR_ACTION_TYPE_BOOLEAN_INPUT},
	                {"recenter_right", XR_ACTION_TYPE_BOOLEAN_INPUT},
	        },
	        .bindings = {
	                suggested_binding{
	                        {
	                                "/interaction_profiles/oculus/touch_controller",
	                                "/interaction_profiles/facebook/touch_controller_pro",
	                                "/interaction_profiles/meta/touch_pro_controller",
	                                "/interaction_profiles/meta/touch_controller_plus",
	                                "/interaction_profiles/meta/touch_plus_controller",
	                                "/interaction_profiles/bytedance/pico_neo3_controller",
	                                "/interaction_profiles/bytedance/pico4_controller",
	                                "/interaction_profiles/bytedance/pico4s_controller",
	                                "/interaction_profiles/htc/vive_focus3_controller",
	                        },
	                        {
	                                {"left_aim", "/user/hand/left/input/aim/pose"},
	                                {"left_trigger", "/user/hand/left/input/trigger/value"},
	                                {"left_squeeze", "/user/hand/left/input/squeeze/value"},
	                                {"left_scroll", "/user/hand/left/input/thumbstick"},
	                                {"left_haptic", "/user/hand/left/output/haptic"},
	                                {"right_aim", "/user/hand/right/input/aim/pose"},
	                                {"right_trigger", "/user/hand/right/input/trigger/value"},
	                                {"right_squeeze", "/user/hand/right/input/squeeze/value"},
	                                {"right_scroll", "/user/hand/right/input/thumbstick"},
	                                {"right_haptic", "/user/hand/right/output/haptic"},

	                                {"recenter_left", "/user/hand/left/input/squeeze/value"},
	                                {"recenter_right", "/user/hand/right/input/squeeze/value"},
	                        },
	                },
	                suggested_binding{
	                        {
	                                "/interaction_profiles/khr/simple_controller",
	                        },
	                        {
	                                {"left_aim", "/user/hand/left/input/aim/pose"},
	                                {"left_trigger", "/user/hand/left/input/select/click"},
	                                {"left_squeeze", "/user/hand/left/input/menu/click"},
	                                {"right_aim", "/user/hand/right/input/aim/pose"},
	                                {"right_trigger", "/user/hand/right/input/select/click"},
	                                {"right_squeeze", "/user/hand/right/input/menu/click"},
	                        },
	                },
	        }};

	return m;
}
