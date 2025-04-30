#include "ip/UdpSocket.h"
#include "osc/OscOutboundPacketStream.h"
#include "plugin.hpp"
#include <atomic>
#include <mutex>
#include <string>

#define DEFAULT_TARGET_IP "127.0.0.1"
#define DEFAULT_TARGET_PORT 9001
#define DEFAULT_OSC_ADDRESS "/telepath/tx"
#define OUTPUT_BUFFER_SIZE 1024

struct OscSender
{
	UdpTransmitSocket* socket = nullptr;
	std::mutex mutex;
	std::atomic<bool> initialized{false};
	std::string targetIp = DEFAULT_TARGET_IP;
	int targetPort = DEFAULT_TARGET_PORT;

	bool setup(const std::string& ip, int port)
	{
		std::lock_guard<std::mutex> lock(mutex);

		if (socket)
		{
			INFO("OSCTX: Replacing existing UDP transmit socket.");
			delete socket;
			socket = nullptr;
		}
		initialized = false;

		targetIp = ip;
		targetPort = port;

		if (targetIp.empty() || targetPort <= 0 || targetPort >= 65536)
		{
			WARN("OSCTX: Invalid target IP (%s) or port (%d). Cannot "
				 "initialize sender.",
				 targetIp.c_str(), targetPort);
			return false;
		}

		try
		{
			INFO("OSCTX: Attempting to create UDP transmit socket for %s:%d",
				 targetIp.c_str(), targetPort);
			socket = new UdpTransmitSocket(
				IpEndpointName(targetIp.c_str(), targetPort));
			if (socket)
			{
				INFO("OSCTX: UDP transmit socket created successfully.");
				initialized = true;
				return true;
			}
			else
			{
				WARN("OSCTX: Failed to create UdpTransmitSocket (returned "
					 "null).");
				return false;
			}
		}
		catch (const std::exception& e)
		{
			WARN("OSCTX: Error initializing UDP transmit socket: %s", e.what());
			delete socket;
			socket = nullptr;
			return false;
		}
		catch (...)
		{
			WARN("OSCTX: Unknown error initializing UDP transmit socket.");
			delete socket;
			socket = nullptr;
			return false;
		}
	}

	void stop()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (socket)
		{
			INFO("OSCTX: Closing UDP transmit socket.");
			delete socket;
			socket = nullptr;
		}
		initialized = false;
	}

	bool send(const char* address, float value)
	{
		if (!initialized.load() || !socket)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(mutex);
		if (!initialized.load(std::memory_order_relaxed) || !socket)
		{
			return false;
		}

		try
		{
			char buffer[OUTPUT_BUFFER_SIZE];
			osc::OutboundPacketStream p(buffer, OUTPUT_BUFFER_SIZE);

			p << osc::BeginMessage(address) << value << osc::EndMessage;

			if (p.IsReady())
			{
				socket->Send(p.Data(), p.Size());
			}
			else
			{
				WARN("OSCTX: OSC packet stream buffer overflow for address %s",
					 address);
				return false;
			}
			return true;
		}
		catch (const std::exception& e)
		{
			WARN("OSCTX: Error sending OSC message (%s): %s", address,
				 e.what());
			return false;
		}
		catch (...)
		{
			WARN("OSCTX: Unknown error sending OSC message (%s).", address);
			return false;
		}
	}

	~OscSender()
	{
		stop();
	}
};

struct OSCTX : Module
{
	enum ParamId
	{
		PARAMS_LEN
	};
	enum InputId
	{
		VALUE_INPUT,
		INPUTS_LEN
	};
	enum OutputId
	{
		OUTPUTS_LEN
	};
	enum LightId
	{
		STATUS_LIGHT,
		LIGHTS_LEN
	};

	OscSender sender;
	std::string targetIp = DEFAULT_TARGET_IP;
	int targetPort = DEFAULT_TARGET_PORT;
	std::string targetAddress = DEFAULT_OSC_ADDRESS;

	float lastSentValue = 0.f;
	bool lastSendSuccess = false;
	float statusPulse = 0.f;

	OSCTX()
	{
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configInput(VALUE_INPUT, "Value");
		if (targetAddress.empty())
			targetAddress = DEFAULT_OSC_ADDRESS;
		if (targetIp.empty())
			targetIp = DEFAULT_TARGET_IP;
		if (targetPort <= 0)
			targetPort = DEFAULT_TARGET_PORT;
	}

	~OSCTX()
	{
		sender.stop();
	}

	json_t* dataToJson() override
	{
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "targetIp", json_string(targetIp.c_str()));
		json_object_set_new(rootJ, "targetPort", json_integer(targetPort));
		json_object_set_new(rootJ, "targetAddress",
							json_string(targetAddress.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override
	{
		json_t* ipJ = json_object_get(rootJ, "targetIp");
		if (ipJ)
			targetIp = json_string_value(ipJ);
		else
			targetIp = DEFAULT_TARGET_IP;


		json_t* portJ = json_object_get(rootJ, "targetPort");
		if (portJ)
			targetPort = json_integer_value(portJ);
		else
			targetPort = DEFAULT_TARGET_PORT;

		json_t* addressJ = json_object_get(rootJ, "targetAddress");
		if (addressJ)
			targetAddress = json_string_value(addressJ);
		else
			targetAddress = DEFAULT_OSC_ADDRESS;

		setupSender();
	}

	void setupSender()
	{
		sender.setup(targetIp, targetPort);
	}

	void setTargetIp(const std::string& newIp)
	{
		if (newIp != targetIp)
		{
			targetIp = newIp;
			setupSender();
		}
	}

	void setTargetPort(int newPort)
	{
		if (newPort != targetPort && newPort > 0 && newPort < 65536)
		{
			targetPort = newPort;
			setupSender();
		}
	}

	void setTargetAddress(const std::string& newAddress)
	{
		if (newAddress != targetAddress)
		{
			targetAddress = newAddress;
		}
	}

	void onAdd() override
	{
		INFO("OSCTX: Module added. Initializing sender for %s:%d",
			 targetIp.c_str(), targetPort);
		setupSender();
	}

	void onRemove() override
	{
		INFO("OSCTX: Module removed. Stopping sender.");
		sender.stop();
	}

	void onReset() override
	{
		targetIp = DEFAULT_TARGET_IP;
		targetPort = DEFAULT_TARGET_PORT;
		targetAddress = DEFAULT_OSC_ADDRESS;
		setupSender();
	}

	void process(const ProcessArgs& args) override
	{
		statusPulse += args.sampleTime * 5.f;
		if (statusPulse > 1.f)
			statusPulse -= 1.f;

		float lightBrightness = 0.f;
		if (sender.initialized.load())
		{
			lightBrightness = 0.2f;
			if (lastSendSuccess)
			{
				lightBrightness = 1.0f;
			}
		}
		lights[STATUS_LIGHT].setBrightness(lightBrightness);


		if (inputs[VALUE_INPUT].isConnected() && sender.initialized.load() &&
			!targetAddress.empty())
		{
			float valueToSend = inputs[VALUE_INPUT].getVoltage() * 0.1f;

			lastSendSuccess = sender.send(targetAddress.c_str(), valueToSend);
			if (lastSendSuccess)
			{
				lastSentValue = valueToSend;
			}

			// static int sampleCount = 0;
			// const int sendInterval = 10;
			// if (++sampleCount >= sendInterval) {
			//     sampleCount = 0;
			//     lastSendSuccess = sender.send(targetAddress.c_str(),
			//     valueToSend); if (lastSendSuccess) lastSentValue =
			//     valueToSend;
			// }
		}
		else
		{
			lastSendSuccess = false;
		}
	}
};

struct TargetIpTextField : ui::TextField
{
	OSCTX* module;
	TargetIpTextField()
	{
		box.size = mm2px(Vec(30.0, 10.0));
		placeholder = DEFAULT_TARGET_IP;
	}
	void onChange(const event::Change& e) override
	{
		if (module)
			module->setTargetIp(text);
		ui::TextField::onChange(e);
	}
};

struct TargetPortTextField : ui::TextField
{
	OSCTX* module;
	TargetPortTextField()
	{
		box.size = mm2px(Vec(18.0, 10.0));
		placeholder = std::to_string(DEFAULT_TARGET_PORT);
	}
	void onChange(const event::Change& e) override
	{
		if (module)
		{
			try
			{
				module->setTargetPort(std::stoi(text));
			}
			catch (...)
			{
				text = std::to_string(module->targetPort);
			}
		}
		ui::TextField::onChange(e);
	}
};

struct TargetAddressTextField : ui::TextField
{
	OSCTX* module;
	TargetAddressTextField()
	{
		box.size = mm2px(Vec(40.797, 10.0));
		placeholder = DEFAULT_OSC_ADDRESS;
	}
	void onChange(const event::Change& e) override
	{
		if (module)
			module->setTargetAddress(text);
		ui::TextField::onChange(e);
	}
};

struct SentValueDisplay : LedDisplayTextField
{
	OSCTX* module;

	void step() override
	{
		LedDisplayTextField::step();
		if (module)
		{
			text = string::f("%.3f", module->lastSentValue);
			color = module->lastSendSuccess ? nvgRGB(0x20, 0xff, 0x20)
											: nvgRGB(0xff, 0x20, 0x20);
		}
	}

	void draw(const DrawArgs& args) override
	{
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(30, 30, 30));
		nvgFill(args.vg);

		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgStrokeColor(args.vg, nvgRGB(60, 70, 80));
		nvgStrokeWidth(args.vg, 1.0);
		nvgStroke(args.vg);

		LedDisplayTextField::draw(args);
	}
};


struct OSCTXWidget : ModuleWidget
{
	OSCTXWidget(OSCTX* module)
	{
		setModule(module);
		setPanel(
			createPanel(asset::plugin(pluginInstance, "res/Transmit.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(
			Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(
			Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(
			createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH,
										  RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		TargetIpTextField* ipField =
			createWidget<TargetIpTextField>(mm2px(Vec(4.978, 39.919)));
		ipField->module = module;
		if (module)
			ipField->text = module->targetIp;
		addChild(ipField);

		TargetPortTextField* portField =
			createWidget<TargetPortTextField>(mm2px(Vec(36.692, 39.919)));
		portField->module = module;
		if (module)
			portField->text = std::to_string(module->targetPort);
		addChild(portField);

		TargetAddressTextField* addressField =
			createWidget<TargetAddressTextField>(mm2px(Vec(4.978, 60.222)));
		addressField->module = module;
		if (module)
			addressField->text = module->targetAddress;
		addChild(addressField);

		SentValueDisplay* valueDisplay = new SentValueDisplay();
		valueDisplay->box.pos = mm2px(Vec(4.978, 79.549));
		valueDisplay->box.size = mm2px(Vec(40.797, 10.0));
		valueDisplay->module = module;
		addChild(valueDisplay);

		addChild(createLightCentered<MediumLight<GreenRedLight>>(
			mm2px(Vec(49.982, 109.462)), module, OSCTX::STATUS_LIGHT));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.478, 109.462)),
												 module, OSCTX::VALUE_INPUT));
	}
};

Model* modelOSCTX = createModel<OSCTX, OSCTXWidget>("Transmit");