#include "ATX.hpp"

#include "plugin.hpp"

#ifdef _WIN32
struct WinsockInitializer
{
	WinsockInitializer()
	{
		WSADATA wsaData;
		int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (result != 0)
		{
			WARN("WSAStartup failed: %d", result);
		}
	}
	~WinsockInitializer()
	{
		WSACleanup();
	}
} winsockInitializer;
#endif

ATX::ATX()
{
	config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
	configInput(L_INPUT, "Audio L");
	configInput(R_INPUT, "Audio R");

	processBuffer.reserve(FLOATS_PER_PACKET);
}

ATX::~ATX()
{
	stopNetworkThread = true;
	queueCv.notify_all();
	if (networkThread.joinable())
	{
		networkThread.join();
	}
	closeSocket();
}

void ATX::onAdd()
{
	Module::onAdd();
	stopNetworkThread = false;
	socketError = false;
	if (!networkThreadRunning)
	{
		if (initializeSocket())
		{
			networkThread = std::thread(&ATX::networkSendLoop, this);
			networkThreadRunning = true;
		}
		else
		{
			socketError = true;
			WARN("ATX: Failed to initialize socket onAdd.");
		}
	}
}

void ATX::onRemove()
{
	Module::onRemove();
	stopNetworkThread = true;
	queueCv.notify_all();

	if (networkThread.joinable())
	{
		try
		{
			networkThread.join();
		}
		catch (const std::system_error& e)
		{
			WARN("Error joining network thread: %s", e.what());
		}
	}
	networkThreadRunning = false;
	closeSocket();

	std::lock_guard<std::mutex> lock(queueMutex);
	std::queue<std::vector<float>> emptyQueue;
	std::swap(bufferQueue, emptyQueue);
}

void ATX::onReset()
{
	Module::onReset();
	processBuffer.clear();
	std::lock_guard<std::mutex> lock(queueMutex);
	std::queue<std::vector<float>> emptyQueue;
	std::swap(bufferQueue, emptyQueue);
	socketError = false;
	packetsSent = 0;
	// closeSocket();
	// initializeSocket();
}

bool ATX::initializeSocket()
{
	if (sockfd != INVALID_SOCKET)
	{
		closeSocket();
	}

	sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sockfd == INVALID_SOCKET)
	{
#ifdef _WIN32
		WARN("ATX: Failed to create socket: %d", WSAGetLastError());
#else
		WARN("ATX: Failed to create socket: %s", strerror(errno));
#endif
		socketInitialized = false;
		socketError = true;
		return false;
	}

	// int sendBuffSize = 65536;
	// setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&sendBuffSize,
	// sizeof(sendBuffSize));

	socketInitialized = true;
	socketError = false;
	updateTargetAddress();
	INFO("ATX: Socket created for %s:%d", targetIP.c_str(), targetPort);
	return true;
}

void ATX::closeSocket()
{
	if (sockfd != INVALID_SOCKET)
	{
		close_socket(sockfd);
		sockfd = INVALID_SOCKET;
		socketInitialized = false;
		INFO("ATX: Socket closed.");
	}
}

void ATX::updateTargetAddress()
{
	if (!socketInitialized)
		return;

	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(targetPort);

	int pton_result =
		inet_pton(AF_INET, targetIP.c_str(), &serverAddr.sin_addr);

	if (pton_result <= 0)
	{
#ifdef _WIN32
		if (pton_result == 0)
			WARN("ATX: Invalid IP address format: %s", targetIP.c_str());
		else
			WARN("ATX: inet_pton failed: %d", WSAGetLastError());
#else
		if (pton_result == 0)
			WARN("ATX: Invalid IP address format: %s", targetIP.c_str());
		else
			WARN("ATX: inet_pton failed: %s", strerror(errno));
#endif
		socketError = true;
	}
	else
	{
		socketError = false;
		INFO("ATX: Target address updated to %s:%d", targetIP.c_str(),
			 targetPort);
	}
}


void ATX::networkSendLoop()
{
	INFO("ATX: Network thread started.");
	std::vector<float> sendBuffer;
	sendBuffer.reserve(FLOATS_PER_PACKET);

	while (!stopNetworkThread)
	{
		{
			std::unique_lock<std::mutex> lock(queueMutex);
			queueCv.wait_for(lock, std::chrono::milliseconds(100), [this] {
				return !bufferQueue.empty() || stopNetworkThread;
			});

			if (stopNetworkThread)
			{
				break;
			}

			if (bufferQueue.empty())
			{
				continue;
			}

			std::swap(sendBuffer, bufferQueue.front());
			bufferQueue.pop();
		}

		if (sendBuffer.empty())
			continue;

		if (sockfd == INVALID_SOCKET || socketError)
		{
			WARN("ATX: Skipping send, socket invalid or error "
				 "state.");
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		ssize_t bytesSent =
			sendto(sockfd, reinterpret_cast<const char*>(sendBuffer.data()),
				   sendBuffer.size() * sizeof(float), 0,
				   (struct sockaddr*)&serverAddr, sizeof(serverAddr));

		if (bytesSent == SOCKET_ERROR)
		{
#ifdef _WIN32
			WARN("ATX: sendto failed: %d", WSAGetLastError());
#else
			WARN("ATX: sendto failed: %s", strerror(errno));
#endif
			socketError = true;
		}
		else if (bytesSent != (ssize_t)(sendBuffer.size() * sizeof(float)))
		{
			WARN("ATX: Could not send entire buffer (%zd bytes "
				 "sent out of %zu)",
				 bytesSent, sendBuffer.size() * sizeof(float));
		}
		else
		{
			socketError = false;
			packetsSent++;
		}

		sendBuffer.clear();

		// std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
	INFO("ATX: Network thread finished.");
	networkThreadRunning = false;
}

void ATX::process(const ProcessArgs& args)
{
	if (params[TOGGLE_TRANSMISSION_PARAM].getValue() >= 1.0f &&
		!lastToggleState)
	{
		isTransmitting = !isTransmitting;
	}
	lastToggleState = (params[TOGGLE_TRANSMISSION_PARAM].getValue() >= 1.0f);

	if (!networkThreadRunning && !stopNetworkThread && !socketError)
	{
		WARN("ATX: Network thread is not running. Attempting "
			 "recovery...");
		onRemove();
		onAdd();
		if (!networkThreadRunning)
		{
			lights[ERROR_LIGHT].setBrightness(1.0f);
			return;
		}
	}
	else if (socketError)
	{
		lights[ERROR_LIGHT].setBrightness(1.0f);
	}
	else
	{
		lights[ERROR_LIGHT].setBrightness(0.0f);
	}

	if (isTransmitting)
	{
		int channels = std::max(1, inputs[L_INPUT].getChannels());
		inputs[R_INPUT].setChannels(channels);

		int samplesToProcess =
			static_cast<int>(args.sampleRate * args.sampleTime);
		for (int i = 0; i < samplesToProcess; ++i)
		{
			// TODO: polyphony
			float left = inputs[L_INPUT].getVoltage(0);
			float right = inputs[R_INPUT].isConnected()
							  ? inputs[R_INPUT].getVoltage(0)
							  : left;

			processBuffer.push_back(left);
			processBuffer.push_back(right);

			if (processBuffer.size() >= FLOATS_PER_PACKET)
			{
				{
					std::lock_guard<std::mutex> lock(queueMutex);
					const size_t MAX_QUEUE_DEPTH = 10;
					if (bufferQueue.size() < MAX_QUEUE_DEPTH)
					{
						bufferQueue.emplace(std::move(processBuffer));
					}
					else
					{
						WARN("ATX: Network queue full, dropping audio data.");
						processBuffer.clear();
						processBuffer.reserve(FLOATS_PER_PACKET);
					}
				}

				queueCv.notify_one();

				processBuffer.reserve(FLOATS_PER_PACKET);
			}
		}
	}
	else
	{
		processBuffer.clear();
		processBuffer.reserve(FLOATS_PER_PACKET);
	}


	const int PACKETS_PER_BLINK = 5;
	int currentPackets = packetsSent.load();
	static int lastPacketCount = 0;
	if (currentPackets / PACKETS_PER_BLINK !=
		lastPacketCount / PACKETS_PER_BLINK)
	{
		lights[SENDING_LIGHT].setBrightness(1.0f);
	}
	else
	{
		lights[SENDING_LIGHT].setBrightnessSmooth(0.0f, args.sampleTime * 10.f);
	}
	lastPacketCount = currentPackets;
}

json_t* ATX::dataToJson()
{
	json_t* rootJ = json_object();
	json_object_set_new(rootJ, "targetIP", json_string(targetIP.c_str()));
	json_object_set_new(rootJ, "targetPort", json_integer(targetPort));
	json_object_set_new(rootJ, "isTransmitting", json_boolean(isTransmitting));
	return rootJ;
}

void ATX::dataFromJson(json_t* rootJ)
{
	json_t* ipJ = json_object_get(rootJ, "targetIP");
	if (ipJ && json_is_string(ipJ))
	{
		targetIP = json_string_value(ipJ);
	}

	json_t* portJ = json_object_get(rootJ, "targetPort");
	if (portJ && json_is_integer(portJ))
	{
		targetPort = json_integer_value(portJ);
		if (targetPort <= 0 || targetPort > 65535)
		{
			WARN("ATX: Loaded invalid port %d, reverting to "
				 "default 7002.",
				 targetPort);
			targetPort = 7002;
		}
	}

	json_t* isTransmittingJ = json_object_get(rootJ, "isTransmitting");
	if (isTransmittingJ && json_is_boolean(isTransmittingJ))
	{
		isTransmitting = json_boolean_value(isTransmittingJ);
	}

	updateTargetAddress();
}

ATXWidget::ATXWidget(ATX* module)
{
	setModule(module);
	setPanel(createPanel(asset::plugin(pluginInstance, "res/Stream.svg")));

	addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
	addChild(
		createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
	addChild(createWidget<ScrewSilver>(
		Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
	addChild(createWidget<ScrewSilver>(Vec(
		box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

	ipTextField = new IPTextField();
	ipTextField->box.pos = mm2px(Vec(4.978, 39.919));
	ipTextField->box.size = mm2px(Vec(30, 10));
	ipTextField->module = module;
	if (module)
		ipTextField->setText(module->targetIP);
	addChild(ipTextField);

	portTextField = new PortTextField();
	portTextField->box.pos = mm2px(Vec(4.978, 60.222));
	portTextField->box.size = mm2px(Vec(18, 10));
	portTextField->module = module;
	if (module)
		portTextField->setText(std::to_string(module->targetPort));
	addChild(portTextField);

	addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.478, 109.462)),
											 module, ATX::L_INPUT));
	addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.162, 109.462)),
											 module, ATX::R_INPUT));

	addChild(createLightCentered<SmallLight<GreenLight>>(
		mm2px(Vec(20.314, 99.233)), module, ATX::SENDING_LIGHT));
	addChild(createLightCentered<SmallLight<RedLight>>(
		mm2px(Vec(30.162, 66.783)), module, ATX::ERROR_LIGHT));

	addParam(createParamCentered<CKD6>(mm2px(Vec(20.314, 91.746)), module,
									   ATX::TOGGLE_TRANSMISSION_PARAM));
}

void ATXWidget::step()
{
	if (module)
	{
		auto* senderModule = dynamic_cast<ATX*>(module);
		if (senderModule)
		{
			if (ipTextField && ipTextField->getText() != senderModule->targetIP)
			{
				ipTextField->setText(senderModule->targetIP);
			}
			if (portTextField && portTextField->getText() !=
									 std::to_string(senderModule->targetPort))
			{
				portTextField->setText(
					std::to_string(senderModule->targetPort));
			}
		}
	}
	ModuleWidget::step();
}

Model* modelATX = createModel<ATX, ATXWidget>("ATX");