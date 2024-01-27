#include "isobus/hardware_integration/available_can_drivers.hpp"
#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/isobus/can_general_parameter_group_numbers.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_partnered_control_function.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_standard_data_description_indices.hpp"
#include "isobus/isobus/isobus_task_controller_server.hpp"

#include "console_logger.cpp"

#include <atomic>
#include <csignal>

//! It is discouraged to use global variables, but it is done here for simplicity.
static std::atomic_bool running = { true };
static std::shared_ptr<isobus::ControlFunction> clientTC = nullptr;

using namespace std;

void signal_handler(int)
{
	running = false;
}

// Define a very basic TC server.
// You can use this as a starting point for your own TC server!
// You'll have to implement the functions here to make it "do" something.
class MyTCServer : public isobus::TaskControllerServer
{
public:
	MyTCServer(std::shared_ptr<isobus::InternalControlFunction> internalControlFunction,
	           std::uint8_t numberBoomsSupported,
	           std::uint8_t numberSectionsSupported,
	           std::uint8_t numberChannelsSupportedForPositionBasedControl,
	           std::uint8_t optionsBitfield) :
	  TaskControllerServer(internalControlFunction,
	                       numberBoomsSupported,
	                       numberSectionsSupported,
	                       numberChannelsSupportedForPositionBasedControl,
	                       optionsBitfield)
	{
	}

	bool activate_object_pool(std::shared_ptr<isobus::ControlFunction> client, ObjectPoolActivationError &, ObjectPoolErrorCodes &, std::uint16_t &, std::uint16_t &) override
	{
		clientTC = client;
		return true;
	}

	bool change_designator(std::shared_ptr<isobus::ControlFunction>, std::uint16_t, const std::vector<std::uint8_t> &)
	{
		return true;
	}

	bool deactivate_object_pool(std::shared_ptr<isobus::ControlFunction>)
	{
		return true;
	}

	bool delete_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction>, ObjectPoolDeletionErrors &)
	{
		return true;
	}

	bool get_is_stored_device_descriptor_object_pool_by_structure_label(std::shared_ptr<isobus::ControlFunction>, const std::vector<std::uint8_t> &, const std::vector<std::uint8_t> &)
	{
		return false;
	}

	bool get_is_stored_device_descriptor_object_pool_by_localization_label(std::shared_ptr<isobus::ControlFunction>, const std::array<std::uint8_t, 7> &)
	{
		return false;
	}

	bool get_is_enough_memory_available(std::uint32_t)
	{
		return true;
	}

	std::uint32_t get_number_of_complete_object_pools_stored_for_client(std::shared_ptr<isobus::ControlFunction>)
	{
		return 0;
	}

	void identify_task_controller(std::uint8_t)
	{
	}

	void on_client_timeout(std::shared_ptr<isobus::ControlFunction>)
	{
	}

	void on_process_data_acknowledge(std::shared_ptr<isobus::ControlFunction>, std::uint16_t, std::uint16_t, std::uint8_t, ProcessDataCommands)
	{
	}

	bool on_value_command(std::shared_ptr<isobus::ControlFunction>, std::uint16_t, std::uint16_t, std::int32_t, std::uint8_t &)
	{
		return true;
	}

	bool store_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction>, const std::vector<std::uint8_t> &, bool)
	{
		return true;
	}
};

int main()
{
	std::signal(SIGINT, signal_handler);

	std::shared_ptr<isobus::CANHardwarePlugin> canDriver = nullptr;
#if defined(ISOBUS_SOCKETCAN_AVAILABLE)
	canDriver = std::make_shared<isobus::SocketCANInterface>("vcan0");
#elif defined(ISOBUS_WINDOWSPCANBASIC_AVAILABLE)
	canDriver = std::make_shared<isobus::PCANBasicWindowsPlugin>(PCAN_USBBUS1);
#elif defined(ISOBUS_WINDOWSINNOMAKERUSB2CAN_AVAILABLE)
	canDriver = std::make_shared<isobus::InnoMakerUSB2CANWindowsPlugin>(0); // CAN0
#elif defined(ISOBUS_MACCANPCAN_AVAILABLE)
	canDriver = std::make_shared<isobus::MacCANPCANPlugin>(PCAN_USBBUS1);
#elif defined(ISOBUS_SYS_TEC_AVAILABLE)
	canDriver = std::make_shared<isobus::SysTecWindowsPlugin>();
#endif
	if (nullptr == canDriver)
	{
		std::cout << "Unable to find a CAN driver. Please make sure you have one of the above drivers installed with the library." << std::endl;
		std::cout << "If you want to use a different driver, please add it to the list above." << std::endl;
		return -1;
	}

	isobus::CANStackLogger::set_can_stack_logger_sink(&logger);
	isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Debug); // Change this to Debug to see more information
	isobus::CANHardwareInterface::set_number_of_can_channels(1);
	isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, canDriver);

	if ((!isobus::CANHardwareInterface::start()) || (!canDriver->get_is_valid()))
	{
		std::cout << "Failed to start hardware interface. The CAN driver might be invalid." << std::endl;
		return -2;
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(250));

	isobus::NAME TestDeviceNAME(0);

	//! Make sure you change these for your device!!!!
	TestDeviceNAME.set_arbitrary_address_capable(true);
	TestDeviceNAME.set_industry_group(2);
	TestDeviceNAME.set_device_class(0);
	TestDeviceNAME.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TaskController));
	TestDeviceNAME.set_identity_number(20);
	TestDeviceNAME.set_ecu_instance(0);
	TestDeviceNAME.set_function_instance(0); // TC #1. If you want to change the TC number, change this.
	TestDeviceNAME.set_device_class_instance(0);
	TestDeviceNAME.set_manufacturer_code(1407);

	auto TestInternalECU = isobus::InternalControlFunction::create(TestDeviceNAME, 247, 0); // The preferred address for a TC is defined in ISO 11783
	MyTCServer server(TestInternalECU, 4, 255, 16, 0x17); // 4 booms, 255 sections, 16 channels, some options configured using isobus::TaskControllerServer::ServerOptions, such as "Supports Documentation"
	auto &languageInterface = server.get_language_command_interface();
	languageInterface.set_language_code("en"); // This is the default, but you can change it if you want
	languageInterface.set_country_code("US"); // This is the default, but you can change it if you want
	server.initialize();

	std::uint64_t lastTime = 0;
	std::array<bool, 6> sectionWorkStates{ false, false, false, false, false, false };
	std::uint8_t sectionIndex = 0;

	while (running)
	{
		// Toggle sections from increasingly from 1 to 6 on/off every 5 seconds
		std::uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		if (currentTime - lastTime > 5000)
		{
			lastTime = currentTime;
			sectionWorkStates[sectionIndex] = !sectionWorkStates[sectionIndex];
			sectionIndex++;
			sectionIndex %= 6;

			// Send the new section work states to the client
			std::uint32_t value = 0;
			for (std::uint8_t i = 0; i < 6; i++)
			{
				value |= (sectionWorkStates[i] ? static_cast<std::uint32_t>(0x01) : static_cast<std::uint32_t>(0x00)) << (2 * i);
			}
			server.send_set_value_and_acknowledge(clientTC, static_cast<std::uint16_t>(isobus::DataDescriptionIndex::SetpointCondensedWorkState1_16), 2, value);
		}

		server.update();

		// Update again in a little bit
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	server.terminate();
	isobus::CANHardwareInterface::stop();
	return 0;
}
