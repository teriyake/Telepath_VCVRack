#include "ip/UdpSocket.h"
#include "osc/OscPacketListener.h"
#include "osc/OscReceivedElements.h"
#include "plugin.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#define DEFAULT_PORT 7001
#define DEFAULT_ADDRESS "/telepath/rx"

struct OscReceiver : osc::OscPacketListener
{
	std::atomic<bool> running{false};
	std::thread thread;
	UdpListeningReceiveSocket* socket = nullptr;
	std::mutex mutex;

	std::string targetAddress = DEFAULT_ADDRESS;
	float lastValue = 0.f;

	std::atomic<bool> changeDetected{false};

	void setTargetAddress(const std::string& address)
	{
		std::lock_guard<std::mutex> lock(mutex);
		targetAddress = address;
		INFO("OSCRX Receiver: Target address set to %s", targetAddress.c_str());
	}

	void setup(int port, const std::string& initialTargetAddress)
	{
		stop();

		try
		{
			setTargetAddress(initialTargetAddress);
			running = true;
			socket = new UdpListeningReceiveSocket(
				IpEndpointName(IpEndpointName::ANY_ADDRESS, port), this);

			thread = std::thread([this]() {
				try
				{
					UdpListeningReceiveSocket* localSocket = socket;
					if (localSocket && running)
					{
						INFO("OSCRX: Starting OSC listener thread at address: "
							 "%s).",
							 targetAddress.c_str());
						localSocket->Run();
						INFO("OSCRX: OSC listener thread finished.");
					}
				}
				catch (const std::exception& e)
				{
					WARN("OSCRX: OSC receiver thread exception: %s", e.what());
					running = false;
				}
			});
		}
		catch (const std::exception& e)
		{
			WARN("OSCRX: Failed to setup OSC receiver: %s", e.what());
			stop();
		}
	}

	void stop()
	{
		running = false;
		if (socket)
		{
			socket->AsynchronousBreak();
		}

		if (thread.joinable())
		{
			thread.join();
		}

		if (socket)
		{
			delete socket;
			socket = nullptr;
		}

		std::lock_guard<std::mutex> lock(mutex);
		targetAddress = "";
		lastValue = 0.f;
		changeDetected = false;
	}

	~OscReceiver()
	{
		stop();
	}

protected:
	virtual void ProcessMessage(const osc::ReceivedMessage& m,
								const IpEndpointName& remoteEndpoint) override
	{
		/*
		char remoteIpStr[IpEndpointName::ADDRESS_STRING_LENGTH];
		remoteEndpoint.AddressAsString(remoteIpStr);
		INFO("OSCRX: ProcessMessage called from %s:%d", remoteIpStr,
			 remoteEndpoint.port);
		INFO("OSCRX: Received OSC Address: %s", m.AddressPattern());
		INFO("OSCRX: Received OSC TypeTags: %s", m.TypeTags());
		*/
		try
		{
			std::string incomingAddress = m.AddressPattern();
			std::string currentTargetAddress;
			{
				std::lock_guard<std::mutex> lock(mutex);
				currentTargetAddress = targetAddress;
			}

			if (currentTargetAddress.empty() ||
				incomingAddress != currentTargetAddress)
			{
				return;
			}

			osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();

			if (arg != m.ArgumentsEnd() && arg->IsFloat())
			{
				float value = arg->AsFloat();
				{
					std::lock_guard<std::mutex> lock(mutex);
					lastValue = value;
				}
				changeDetected.store(true, std::memory_order_seq_cst);
			}
		}
		catch (const osc::Exception& e)
		{
			WARN("OSCRX: Error parsing OSC message: %s (Address: %s)", e.what(),
				 m.AddressPattern());
		}
		catch (const std::exception& e)
		{
			WARN(
				"OSCRX: Standard exception in ProcessMessage: %s (Address: %s)",
				e.what(), m.AddressPattern());
		}
		catch (...)
		{
			WARN("OSCRX: Unknown exception in ProcessMessage (Address: %s)",
				 m.AddressPattern());
		}
	}
};

struct OSCRX : Module
{
	enum ParamId
	{
		PARAMS_LEN
	};
	enum InputId
	{
		INPUTS_LEN
	};
	enum OutputId
	{
		VALUE_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId
	{
		STATUS_LIGHT,
		LIGHTS_LEN
	};

	OscReceiver receiver;

	int port = DEFAULT_PORT;
	std::string targetAddress = DEFAULT_ADDRESS;
	float displayValue = 0.f;
	bool connected = false;
	float connectedPulse = 0.f;

	json_t* dataToJson() override
	{
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "port", json_integer(port));
		json_object_set_new(rootJ, "targetAddress",
							json_string(targetAddress.c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override
	{
		json_t* portJ = json_object_get(rootJ, "port");
		if (portJ)
			port = json_integer_value(portJ);

		json_t* addressJ = json_object_get(rootJ, "targetAddress");
		if (addressJ)
			targetAddress = json_string_value(addressJ);
		else
			targetAddress = DEFAULT_ADDRESS;
	}

	void setupReceiver()
	{
		receiver.stop();
		if (port > 0 && port < 65536 && !targetAddress.empty())
		{
			INFO("OSCRX: Setting up receiver on port %d for address %s", port,
				 targetAddress.c_str());
			try
			{
				receiver.setup(port, targetAddress);
				if (receiver.running)
				{
					INFO("OSCRX: Receiver setup successfully.");
					connected = true;
				}
				else
				{
					WARN("OSCRX: Receiver setup failed, thread did not start.");
					connected = false;
				}
			}
			catch (const std::exception& e)
			{
				WARN("OSCRX: Exception during receiver setup: %s", e.what());
				connected = false;
				receiver.stop();
			}
		}
		else
		{
			if (targetAddress.empty())
			{
				INFO("OSCRX: Receiver not started: Target address is empty.");
			}
			else
			{
				INFO("OSCRX: Receiver not started: Invalid port (%d).", port);
			}
			connected = false;
		}
	}

	OSCRX()
	{
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configOutput(VALUE_OUTPUT, "OSC value");
	}

	~OSCRX()
	{
		receiver.stop();
	}

	void setPort(int newPort)
	{
		if (newPort == port)
			return;

		if (newPort != port && newPort > 0 && newPort < 65536)
		{
			port = newPort;
			setupReceiver();
		}
	}

	void setTargetAddress(const std::string& newAddress)
	{
		if (newAddress != targetAddress)
		{
			targetAddress = newAddress;
			receiver.setTargetAddress(targetAddress);
			// setupReceiver();
		}
	}

	void onAdd() override
	{
		setupReceiver();
	}

	void onRemove() override
	{
		receiver.stop();
		connected = false;
	}

	void onReset() override
	{
		port = DEFAULT_PORT;
		targetAddress = DEFAULT_ADDRESS;
		setupReceiver();
	}

	void process(const ProcessArgs& args) override
	{
		connectedPulse += args.sampleTime * 2.f;
		if (connectedPulse > 1.f)
			connectedPulse = 0.f;

		if (connected)
		{
			lights[STATUS_LIGHT].setBrightness(
				0.5f + 0.5f * std::sin(connectedPulse * 6.28f));
		}
		else
		{
			lights[STATUS_LIGHT].setBrightness(0.f);
		}

		if (receiver.changeDetected.exchange(false, std::memory_order_seq_cst))
		{
			float tempValue;
			{
				std::lock_guard<std::mutex> lock(receiver.mutex);
				tempValue = receiver.lastValue;
			}
			displayValue = tempValue;
		}

		outputs[VALUE_OUTPUT].setVoltage(displayValue * 10.f);
	}
};

struct PortTextField : ui::TextField
{
	OSCRX* module;

	PortTextField()
	{
		box.size = mm2px(Vec(23.0, 10.0));
		placeholder = std::to_string(DEFAULT_PORT);
		text = "";
	}

	void onChange(const event::Change& e) override
	{
		if (module)
		{
			try
			{
				int newPort = std::stoi(text);
				module->setPort(newPort);
			}
			catch (...)
			{
				text = std::to_string(module->port);
			}
		}
		ui::TextField::onChange(e);
	}
};

struct AddressTextField : ui::TextField
{
	OSCRX* module;

	AddressTextField()
	{
		box.size = mm2px(Vec(23.0, 10.0));
		placeholder = DEFAULT_ADDRESS;
		text = "";
	}

	void onChange(const event::Change& e) override
	{
		if (module)
		{
			module->setTargetAddress(text);
		}
		ui::TextField::onChange(e);
	}
};

struct ValueDisplay : LedDisplayTextField
{
	OSCRX* module;

	ValueDisplay() {}

	void step() override
	{
		LedDisplayTextField::step();
		if (module)
		{
			text = string::f("%.3f", module->displayValue);
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

struct OSCRXWidget : ModuleWidget
{
	OSCRXWidget(OSCRX* module)
	{
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Listen.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(
			Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(
			Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(
			createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH,
										  RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addChild(createLightCentered<MediumLight<GreenLight>>(
			mm2px(Vec(10.478, 109.452)), module, OSCRX::STATUS_LIGHT));

		PortTextField* portField =
			createWidget<PortTextField>(mm2px(Vec(4.978, 39.919)));
		portField->module = module;
		if (module)
		{
			portField->text = std::to_string(module->port);
		}
		addChild(portField);

		AddressTextField* addressTextField =
			createWidget<AddressTextField>(mm2px(Vec(4.978, 59.254)));
		addressTextField->module = module;
		if (module)
		{
			addressTextField->text = module->targetAddress;
		}
		addChild(addressTextField);

		ValueDisplay* valueDisplay = new ValueDisplay();
		valueDisplay->box.pos = mm2px(Vec(4.978, 78.581));
		valueDisplay->box.size = mm2px(Vec(23.0, 10.0));
		valueDisplay->module = module;
		addChild(valueDisplay);

		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(29.953, 109.452)), module, OSCRX::VALUE_OUTPUT));
	}
};

Model* modelOSCRX = createModel<OSCRX, OSCRXWidget>("Listen");