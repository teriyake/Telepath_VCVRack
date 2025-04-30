#pragma once

#include "rack.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define close_socket close
#endif

using namespace rack;

struct ATX : Module
{
	enum ParamIds
	{
		TOGGLE_TRANSMISSION_PARAM,
		NUM_PARAMS
	};
	enum InputIds
	{
		L_INPUT,
		R_INPUT,
		NUM_INPUTS
	};
	enum OutputIds
	{
		NUM_OUTPUTS
	};
	enum LightIds
	{
		SENDING_LIGHT,
		ERROR_LIGHT,
		NUM_LIGHTS
	};

	std::string targetIP = "127.0.0.1";
	int targetPort = 7002;

	socket_t sockfd = INVALID_SOCKET;
	struct sockaddr_in serverAddr;
	bool socketInitialized = false;
	std::atomic<bool> networkThreadRunning{false};
	std::thread networkThread;

	std::queue<std::vector<float>> bufferQueue;
	std::mutex queueMutex;
	std::condition_variable queueCv;
	std::atomic<bool> stopNetworkThread{false};

	std::vector<float> processBuffer;
	const size_t SAMPLES_PER_PACKET = 256;
	const size_t FLOATS_PER_PACKET = SAMPLES_PER_PACKET * 2;

	std::atomic<bool> socketError{false};
	std::atomic<int> packetsSent{0};
	bool isTransmitting = false;
	bool lastToggleState = false;

	ATX();
	~ATX() override;

	void process(const ProcessArgs& args) override;
	void onAdd() override;
	void onRemove() override;
	void onReset() override;

	void networkSendLoop();

	bool initializeSocket();
	void closeSocket();
	void updateTargetAddress();

	json_t* dataToJson() override;
	void dataFromJson(json_t* rootJ) override;
};

namespace rack
{
namespace app
{
struct ModuleWidget;
} // namespace app
} // namespace rack
namespace rack
{
namespace app
{
namespace widgets
{
struct TextField;
} // namespace widgets
} // namespace app
} // namespace rack

struct IPTextField : ui::TextField
{
	ATX* module;

	void onChange(const ChangeEvent& e) override
	{
		if (module)
		{
			module->targetIP = getText();
			module->updateTargetAddress();
		}
	}
};

struct PortTextField : ui::TextField
{
	ATX* module;

	void onChange(const ChangeEvent& e) override
	{
		if (module)
		{
			try
			{
				int newPort = std::stoi(getText());
				if (newPort > 0 && newPort <= 65535)
				{
					module->targetPort = newPort;
					module->updateTargetAddress();
				}
				else
				{
					WARN("Invalid port number entered: %d. Must be 1-65535.",
						 newPort);
				}
			}
			catch (const std::invalid_argument&)
			{
				WARN("Invalid port format entered.");
			}
			catch (const std::out_of_range&)
			{
				WARN("Port number out of range.");
			}
		}
	}
	/*
	bool onKey(const KeyEvent& e) override {
		if (e.key >= GLFW_KEY_0 && e.key <= GLFW_KEY_9) {
			return TextField::onKey(e);
		}
		if (e.key == GLFW_KEY_BACKSPACE || e.key == GLFW_KEY_DELETE ||
			e.key == GLFW_KEY_LEFT || e.key == GLFW_KEY_RIGHT ||
			e.key == GLFW_KEY_HOME || e.key == GLFW_KEY_END) {
			return TextField::onKey(e);
		}
	GLFW_KEY_V) { ... }

		return false;
	}
	*/
};

extern Model* modelATX;

struct ATXWidget : app::ModuleWidget
{
	IPTextField* ipTextField = nullptr;
	PortTextField* portTextField = nullptr;

	ATXWidget(ATX* module);
	void step() override;
};