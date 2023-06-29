#include <gtest/gtest.h>

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/virtual_can_plugin.hpp"
#include "isobus/isobus/isobus_diagnostic_protocol.hpp"
#include "isobus/utility/system_timing.hpp"

using namespace isobus;

TEST(DIAGNOSTIC_PROTOCOL_TESTS, CreateAndDestroyProtocolObjects)
{
	NAME TestDeviceNAME(0);
	auto TestInternalECU = InternalControlFunction::create(TestDeviceNAME, 0x1C, 0);

	auto diagnosticProtocol = std::make_unique<DiagnosticProtocol>(TestInternalECU);
	EXPECT_NO_THROW(diagnosticProtocol->initialize());

	EXPECT_NO_THROW(diagnosticProtocol->terminate());
	diagnosticProtocol.reset();

	//! @todo try to reduce the reference count, such that that we don't use a control function after it is destroyed
	ASSERT_TRUE(TestInternalECU->destroy(2));
}

TEST(DIAGNOSTIC_PROTOCOL_TESTS, MessageEncoding)
{
	NAME TestDeviceNAME(0);

	TestDeviceNAME.set_arbitrary_address_capable(true);
	TestDeviceNAME.set_industry_group(2);
	TestDeviceNAME.set_device_class(6);
	TestDeviceNAME.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::DriveAxleControlBrakes));
	TestDeviceNAME.set_identity_number(2);
	TestDeviceNAME.set_ecu_instance(0);
	TestDeviceNAME.set_function_instance(0);
	TestDeviceNAME.set_device_class_instance(0);
	TestDeviceNAME.set_manufacturer_code(64);

	auto TestInternalECU = InternalControlFunction::create(TestDeviceNAME, 0xAA, 0);

	DiagnosticProtocol protocolUnderTest(TestInternalECU, DiagnosticProtocol::NetworkType::SAEJ1939Network1PrimaryVehicleNetwork);

	EXPECT_FALSE(protocolUnderTest.get_initialized());
	protocolUnderTest.initialize();
	EXPECT_TRUE(protocolUnderTest.get_initialized());

	VirtualCANPlugin testPlugin;
	testPlugin.open();

	CANHardwareInterface::set_number_of_can_channels(1);
	CANHardwareInterface::assign_can_channel_frame_handler(0, std::make_shared<VirtualCANPlugin>());
	CANHardwareInterface::start();

	std::uint32_t waitingTimestamp_ms = SystemTiming::get_timestamp_ms();

	while ((!TestInternalECU->get_address_valid()) &&
	       (!SystemTiming::time_expired_ms(waitingTimestamp_ms, 2000)))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	ASSERT_TRUE(TestInternalECU->get_address_valid());

	CANMessageFrame testFrame;

	testFrame.timestamp_us = 0;
	testFrame.identifier = 0;
	testFrame.channel = 0;
	std::memset(testFrame.data, 0, sizeof(testFrame.data));
	testFrame.dataLength = 0;
	testFrame.isExtendedFrame = true;

	// Force claim some other partner
	testFrame.dataLength = 8;
	testFrame.channel = 0;
	testFrame.isExtendedFrame = true;
	testFrame.identifier = 0x18EEFFAB;
	testFrame.data[0] = 0x04;
	testFrame.data[1] = 0x05;
	testFrame.data[2] = 0x07;
	testFrame.data[3] = 0x12;
	testFrame.data[4] = 0x01;
	testFrame.data[5] = 0x82;
	testFrame.data[6] = 0x01;
	testFrame.data[7] = 0xA0;
	CANNetworkManager::process_receive_can_message_frame(testFrame);

	// Get the virtual CAN plugin back to a known state
	while (!testPlugin.get_queue_empty())
	{
		testPlugin.read_frame(testFrame);
	}
	ASSERT_TRUE(testPlugin.get_queue_empty());

	// Ready to run some tests
	std::cerr << "These tests use BAM to transmit, so they may take several seconds.." << std::endl;

	{
		// Test ECU ID format against J1939-71
		protocolUnderTest.set_ecu_id_field(isobus::DiagnosticProtocol::ECUIdentificationFields::HardwareID, "Some Hardware ID");
		protocolUnderTest.set_ecu_id_field(isobus::DiagnosticProtocol::ECUIdentificationFields::Location, "The Internet");
		protocolUnderTest.set_ecu_id_field(isobus::DiagnosticProtocol::ECUIdentificationFields::ManufacturerName, "None");
		protocolUnderTest.set_ecu_id_field(isobus::DiagnosticProtocol::ECUIdentificationFields::PartNumber, "1234");
		protocolUnderTest.set_ecu_id_field(isobus::DiagnosticProtocol::ECUIdentificationFields::SerialNumber, "9876");
		protocolUnderTest.set_ecu_id_field(isobus::DiagnosticProtocol::ECUIdentificationFields::Type, "AgISOStack");

		// Use a PGN request to trigger sending it from the protocol
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xC5;
		testFrame.data[1] = 0xFD;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();

		// Make sure we're using ISO mode for this parsing to work
		ASSERT_FALSE(protocolUnderTest.get_j1939_mode());

		// This message gets sent with BAM with PGN 0xFDC5, so we'll have to wait a while for the message to send.
		// This a a nice test because it exercises the transport protocol as well
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		std::uint16_t expectedBAMLength = 56; // This is all strings lengths plus delimiters

		// Broadcast Announce Message
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18ECFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x20, testFrame.data[0]); // BAM Multiplexer
		EXPECT_EQ(expectedBAMLength & 0xFF, testFrame.data[1]); // Length LSB
		EXPECT_EQ((expectedBAMLength >> 8) & 0xFF, testFrame.data[2]); // Length MSB
		EXPECT_EQ(0x08, testFrame.data[3]); // Number of frames in session (based on length)
		EXPECT_EQ(0xFF, testFrame.data[4]); // Always 0xFF
		EXPECT_EQ(0xC5, testFrame.data[5]); // PGN LSB
		EXPECT_EQ(0xFD, testFrame.data[6]); // PGN
		EXPECT_EQ(0x00, testFrame.data[7]); // PGN MSB

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 1
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // Sequence 1
		EXPECT_EQ('1', testFrame.data[1]); // Part Number index 0
		EXPECT_EQ('2', testFrame.data[2]); // Part Number index 1
		EXPECT_EQ('3', testFrame.data[3]); // Part Number index 2
		EXPECT_EQ('4', testFrame.data[4]); // Part Number index 3
		EXPECT_EQ('*', testFrame.data[5]); // Delimiter
		EXPECT_EQ('9', testFrame.data[6]); // Serial number index 0
		EXPECT_EQ('8', testFrame.data[7]); // Serial number index 1

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 2
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x02, testFrame.data[0]); // Sequence 2
		EXPECT_EQ('7', testFrame.data[1]); // Serial number index 2
		EXPECT_EQ('6', testFrame.data[2]); // Serial number index 3
		EXPECT_EQ('*', testFrame.data[3]); // Delimiter
		EXPECT_EQ('T', testFrame.data[4]); // Location index 0
		EXPECT_EQ('h', testFrame.data[5]); // Location index 1
		EXPECT_EQ('e', testFrame.data[6]); // Location index 2
		EXPECT_EQ(' ', testFrame.data[7]); // Location index 3

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 3
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x03, testFrame.data[0]); // Sequence 3
		EXPECT_EQ('I', testFrame.data[1]); // Location index 4
		EXPECT_EQ('n', testFrame.data[2]); // Location index 5
		EXPECT_EQ('t', testFrame.data[3]); // Location index 6
		EXPECT_EQ('e', testFrame.data[4]); // Location index 7
		EXPECT_EQ('r', testFrame.data[5]); // Location index 8
		EXPECT_EQ('n', testFrame.data[6]); // Location index 9
		EXPECT_EQ('e', testFrame.data[7]); // Location index 10

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 4
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x04, testFrame.data[0]); // Sequence 4
		EXPECT_EQ('t', testFrame.data[1]); // Location index 11
		EXPECT_EQ('*', testFrame.data[2]); // Delimiter
		EXPECT_EQ('A', testFrame.data[3]); // Type Index 0
		EXPECT_EQ('g', testFrame.data[4]); // Type Index 1
		EXPECT_EQ('I', testFrame.data[5]); // Type Index 2
		EXPECT_EQ('S', testFrame.data[6]); // Type Index 3
		EXPECT_EQ('O', testFrame.data[7]); // Type Index 4

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 5
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x05, testFrame.data[0]); // Sequence 5
		EXPECT_EQ('S', testFrame.data[1]); // Type Index 5
		EXPECT_EQ('t', testFrame.data[2]); // Type Index 6
		EXPECT_EQ('a', testFrame.data[3]); // Type Index 7
		EXPECT_EQ('c', testFrame.data[4]); // Type Index 8
		EXPECT_EQ('k', testFrame.data[5]); // Type Index 9
		EXPECT_EQ('*', testFrame.data[6]); // Delimiter
		EXPECT_EQ('N', testFrame.data[7]); // Manufacturer index 0

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 6
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x06, testFrame.data[0]); // Sequence 6
		EXPECT_EQ('o', testFrame.data[1]); // Manufacturer index 1
		EXPECT_EQ('n', testFrame.data[2]); // Manufacturer index 2
		EXPECT_EQ('e', testFrame.data[3]); // Manufacturer index 3
		EXPECT_EQ('*', testFrame.data[4]); // Delimiter
		EXPECT_EQ('S', testFrame.data[5]); // Hardware ID Index 0
		EXPECT_EQ('o', testFrame.data[6]); // Hardware ID Index 1
		EXPECT_EQ('m', testFrame.data[7]); // Hardware ID Index 2

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 7
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x07, testFrame.data[0]); // Sequence 7
		EXPECT_EQ('e', testFrame.data[1]); // Hardware ID Index 3
		EXPECT_EQ(' ', testFrame.data[2]); // Hardware ID Index 4
		EXPECT_EQ('H', testFrame.data[3]); // Hardware ID Index 5
		EXPECT_EQ('a', testFrame.data[4]); // Hardware ID Index 6
		EXPECT_EQ('r', testFrame.data[5]); // Hardware ID Index 7
		EXPECT_EQ('d', testFrame.data[6]); // Hardware ID Index 8
		EXPECT_EQ('w', testFrame.data[7]); // Hardware ID Index 9

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 7
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x08, testFrame.data[0]); // Sequence 8
		EXPECT_EQ('a', testFrame.data[1]); // Hardware ID Index 10
		EXPECT_EQ('r', testFrame.data[2]); // Hardware ID Index 11
		EXPECT_EQ('e', testFrame.data[3]); // Hardware ID Index 12
		EXPECT_EQ(' ', testFrame.data[4]); // Hardware ID Index 13
		EXPECT_EQ('I', testFrame.data[5]); // Hardware ID Index 14
		EXPECT_EQ('D', testFrame.data[6]); // Hardware ID Index 15
		EXPECT_EQ('*', testFrame.data[7]); // Delimiter (end of the message)
	}

	{
		// Re-test in J1939 mode. Should omit the hardware ID
		protocolUnderTest.set_j1939_mode(true);

		// Use a PGN request to trigger sending it from the protocol
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xC5;
		testFrame.data[1] = 0xFD;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();

		protocolUnderTest.update();

		// Make sure we're using ISO mode for this parsing to work
		ASSERT_TRUE(protocolUnderTest.get_j1939_mode());

		// This message gets sent with BAM with PGN 0xFDC5, so we'll have to wait a while for the message to send.
		// This a a nice test because it exercises the transport protocol as well
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		std::uint16_t expectedBAMLength = 39; // This is all strings lengths plus delimiters

		// Broadcast Announce Message
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18ECFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x20, testFrame.data[0]); // BAM Multiplexer
		EXPECT_EQ(expectedBAMLength & 0xFF, testFrame.data[1]); // Length LSB
		EXPECT_EQ((expectedBAMLength >> 8) & 0xFF, testFrame.data[2]); // Length MSB
		EXPECT_EQ(0x06, testFrame.data[3]); // Number of frames in session (based on length)
		EXPECT_EQ(0xFF, testFrame.data[4]); // Always 0xFF
		EXPECT_EQ(0xC5, testFrame.data[5]); // PGN LSB
		EXPECT_EQ(0xFD, testFrame.data[6]); // PGN
		EXPECT_EQ(0x00, testFrame.data[7]); // PGN MSB

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		// BAM Payload Frame 1
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // Sequence 1
		EXPECT_EQ('1', testFrame.data[1]); // Part Number index 0
		EXPECT_EQ('2', testFrame.data[2]); // Part Number index 1
		EXPECT_EQ('3', testFrame.data[3]); // Part Number index 2
		EXPECT_EQ('4', testFrame.data[4]); // Part Number index 3
		EXPECT_EQ('*', testFrame.data[5]); // Delimiter
		EXPECT_EQ('9', testFrame.data[6]); // Serial number index 0
		EXPECT_EQ('8', testFrame.data[7]); // Serial number index 1

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		// BAM Payload Frame 2
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x02, testFrame.data[0]); // Sequence 2
		EXPECT_EQ('7', testFrame.data[1]); // Serial number index 2
		EXPECT_EQ('6', testFrame.data[2]); // Serial number index 3
		EXPECT_EQ('*', testFrame.data[3]); // Delimiter
		EXPECT_EQ('T', testFrame.data[4]); // Location index 0
		EXPECT_EQ('h', testFrame.data[5]); // Location index 1
		EXPECT_EQ('e', testFrame.data[6]); // Location index 2
		EXPECT_EQ(' ', testFrame.data[7]); // Location index 3

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		// BAM Payload Frame 3
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x03, testFrame.data[0]); // Sequence 3
		EXPECT_EQ('I', testFrame.data[1]); // Location index 4
		EXPECT_EQ('n', testFrame.data[2]); // Location index 5
		EXPECT_EQ('t', testFrame.data[3]); // Location index 6
		EXPECT_EQ('e', testFrame.data[4]); // Location index 7
		EXPECT_EQ('r', testFrame.data[5]); // Location index 8
		EXPECT_EQ('n', testFrame.data[6]); // Location index 9
		EXPECT_EQ('e', testFrame.data[7]); // Location index 10

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		// BAM Payload Frame 4
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x04, testFrame.data[0]); // Sequence 4
		EXPECT_EQ('t', testFrame.data[1]); // Location index 11
		EXPECT_EQ('*', testFrame.data[2]); // Delimiter
		EXPECT_EQ('A', testFrame.data[3]); // Type Index 0
		EXPECT_EQ('g', testFrame.data[4]); // Type Index 1
		EXPECT_EQ('I', testFrame.data[5]); // Type Index 2
		EXPECT_EQ('S', testFrame.data[6]); // Type Index 3
		EXPECT_EQ('O', testFrame.data[7]); // Type Index 4

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		// BAM Payload Frame 5
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x05, testFrame.data[0]); // Sequence 5
		EXPECT_EQ('S', testFrame.data[1]); // Type Index 5
		EXPECT_EQ('t', testFrame.data[2]); // Type Index 6
		EXPECT_EQ('a', testFrame.data[3]); // Type Index 7
		EXPECT_EQ('c', testFrame.data[4]); // Type Index 8
		EXPECT_EQ('k', testFrame.data[5]); // Type Index 9
		EXPECT_EQ('*', testFrame.data[6]); // Delimiter
		EXPECT_EQ('N', testFrame.data[7]); // Manufacturer index 0

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// DM1 might be sent in j1939 mode, need to screen it out
		if (((testFrame.identifier >> 8) & 0xFFFF) == 0xFECA)
		{
			EXPECT_TRUE(testPlugin.read_frame(testFrame));
		}

		// BAM Payload Frame 6
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x06, testFrame.data[0]); // Sequence 6
		EXPECT_EQ('o', testFrame.data[1]); // Manufacturer index 1
		EXPECT_EQ('n', testFrame.data[2]); // Manufacturer index 2
		EXPECT_EQ('e', testFrame.data[3]); // Manufacturer index 3
		EXPECT_EQ('*', testFrame.data[4]); // Delimiter
		EXPECT_EQ(0xFF, testFrame.data[5]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding

		protocolUnderTest.set_j1939_mode(false);
		EXPECT_FALSE(protocolUnderTest.get_j1939_mode());
	}

	{
		/// Now, test software ID against J1939-71
		protocolUnderTest.set_software_id_field(0, "Unit Test 1.0.0");
		protocolUnderTest.set_software_id_field(1, "Another version x.x.x.x");

		// Use a PGN request to trigger sending it from the protocol
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xDA;
		testFrame.data[1] = 0xFE;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();

		protocolUnderTest.update();

		// This message gets sent with BAM, so we'll have to wait a while for the message to send.
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		std::uint16_t expectedBAMLength = 40; // This is all strings lengths plus delimiters

		// Broadcast Announce Message
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18ECFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x20, testFrame.data[0]); // BAM Multiplexer
		EXPECT_EQ(expectedBAMLength & 0xFF, testFrame.data[1]); // Length LSB
		EXPECT_EQ((expectedBAMLength >> 8) & 0xFF, testFrame.data[2]); // Length MSB
		EXPECT_EQ(0x06, testFrame.data[3]); // Number of frames in session (based on length)
		EXPECT_EQ(0xFF, testFrame.data[4]); // Always 0xFF
		EXPECT_EQ(0xDA, testFrame.data[5]); // PGN LSB
		EXPECT_EQ(0xFE, testFrame.data[6]); // PGN
		EXPECT_EQ(0x00, testFrame.data[7]); // PGN MSB

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 1
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // Sequence 1
		EXPECT_EQ('U', testFrame.data[1]); // Version 0, index 0
		EXPECT_EQ('n', testFrame.data[2]); // Version 0, index 1
		EXPECT_EQ('i', testFrame.data[3]); // Version 0, index 2
		EXPECT_EQ('t', testFrame.data[4]); // Version 0, index 3
		EXPECT_EQ(' ', testFrame.data[5]); // Version 0, index 4
		EXPECT_EQ('T', testFrame.data[6]); // Version 0, index 5
		EXPECT_EQ('e', testFrame.data[7]); // Version 0, index 6

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 2
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x02, testFrame.data[0]); // Sequence 2
		EXPECT_EQ('s', testFrame.data[1]); // Version 0, index 7
		EXPECT_EQ('t', testFrame.data[2]); // Version 0, index 8
		EXPECT_EQ(' ', testFrame.data[3]); // Version 0, index 9
		EXPECT_EQ('1', testFrame.data[4]); // Version 0, index 10
		EXPECT_EQ('.', testFrame.data[5]); // Version 0, index 11
		EXPECT_EQ('0', testFrame.data[6]); // Version 0, index 12
		EXPECT_EQ('.', testFrame.data[7]); // Version 0, index 13

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 3
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x03, testFrame.data[0]); // Sequence 3
		EXPECT_EQ('0', testFrame.data[1]); // Version 0, index 7
		EXPECT_EQ('*', testFrame.data[2]); // Delimiter
		EXPECT_EQ('A', testFrame.data[3]); // Version 1, index 0
		EXPECT_EQ('n', testFrame.data[4]); // Version 1, index 1
		EXPECT_EQ('o', testFrame.data[5]); // Version 1, index 2
		EXPECT_EQ('t', testFrame.data[6]); // Version 1, index 3
		EXPECT_EQ('h', testFrame.data[7]); // Version 1, index 4

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 4
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x04, testFrame.data[0]); // Sequence 4
		EXPECT_EQ('e', testFrame.data[1]); // Version 0, index 7
		EXPECT_EQ('r', testFrame.data[2]); // Delimiter
		EXPECT_EQ(' ', testFrame.data[3]); // Version 1, index 5
		EXPECT_EQ('v', testFrame.data[4]); // Version 1, index 6
		EXPECT_EQ('e', testFrame.data[5]); // Version 1, index 7
		EXPECT_EQ('r', testFrame.data[6]); // Version 1, index 8
		EXPECT_EQ('s', testFrame.data[7]); // Version 1, index 9

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 5
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x05, testFrame.data[0]); // Sequence 5
		EXPECT_EQ('i', testFrame.data[1]); // Version 0, index 7
		EXPECT_EQ('o', testFrame.data[2]); // Delimiter
		EXPECT_EQ('n', testFrame.data[3]); // Version 1, index 5
		EXPECT_EQ(' ', testFrame.data[4]); // Version 1, index 6
		EXPECT_EQ('x', testFrame.data[5]); // Version 1, index 7
		EXPECT_EQ('.', testFrame.data[6]); // Version 1, index 8
		EXPECT_EQ('x', testFrame.data[7]); // Version 1, index 9

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 6
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x06, testFrame.data[0]); // Sequence 6
		EXPECT_EQ('.', testFrame.data[1]); // Version 0, index 10
		EXPECT_EQ('x', testFrame.data[2]); // Version 0, index 11
		EXPECT_EQ('.', testFrame.data[3]); // Version 1, index 12
		EXPECT_EQ('x', testFrame.data[4]); // Version 1, index 13
		EXPECT_EQ('*', testFrame.data[5]); // Delimiter
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding
	}

	{
		// Test diagnostic protocol identification message
		// Use a PGN request to trigger sending it from the protocol
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0x32;
		testFrame.data[1] = 0xFD;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();

		protocolUnderTest.update();

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18FD32AA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // J1939�73
		EXPECT_EQ(0xFF, testFrame.data[1]); // Reserved
		EXPECT_EQ(0xFF, testFrame.data[2]); // Reserved
		EXPECT_EQ(0xFF, testFrame.data[3]); // Reserved
		EXPECT_EQ(0xFF, testFrame.data[4]); // Reserved
		EXPECT_EQ(0xFF, testFrame.data[5]); // Reserved
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding
	}

	{
		// Test Product Identification
		protocolUnderTest.set_product_identification_code("1234567890ABC");
		protocolUnderTest.set_product_identification_brand("Open-Agriculture");
		protocolUnderTest.set_product_identification_model("AgIsoStack++");
		// Use a PGN request to trigger sending it
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0x8D;
		testFrame.data[1] = 0xFC;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();

		protocolUnderTest.update();

		// This message gets sent with BAM, so we'll have to wait a while for the message to send.
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// More manual BAM parsing...
		std::uint16_t expectedBAMLength = 44; // This is all strings lengths plus delimiters

		// Broadcast Announce Message
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18ECFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x20, testFrame.data[0]); // BAM Multiplexer
		EXPECT_EQ(expectedBAMLength & 0xFF, testFrame.data[1]); // Length LSB
		EXPECT_EQ((expectedBAMLength >> 8) & 0xFF, testFrame.data[2]); // Length MSB
		EXPECT_EQ(0x07, testFrame.data[3]); // Number of frames in session (based on length)
		EXPECT_EQ(0xFF, testFrame.data[4]); // Always 0xFF
		EXPECT_EQ(0x8D, testFrame.data[5]); // PGN LSB
		EXPECT_EQ(0xFC, testFrame.data[6]); // PGN
		EXPECT_EQ(0x00, testFrame.data[7]); // PGN MSB

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 1
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // Sequence 1
		EXPECT_EQ('1', testFrame.data[1]); // ID Code index 0
		EXPECT_EQ('2', testFrame.data[2]); // ID Code index 1
		EXPECT_EQ('3', testFrame.data[3]); // ID Code index 2
		EXPECT_EQ('4', testFrame.data[4]); // ID Code index 3
		EXPECT_EQ('5', testFrame.data[5]); // ID Code index 4
		EXPECT_EQ('6', testFrame.data[6]); // ID Code index 5
		EXPECT_EQ('7', testFrame.data[7]); // ID Code index 6

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 2
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x02, testFrame.data[0]); // Sequence 2
		EXPECT_EQ('8', testFrame.data[1]); // ID Code index 7
		EXPECT_EQ('9', testFrame.data[2]); // ID Code index 8
		EXPECT_EQ('0', testFrame.data[3]); // ID Code index 9
		EXPECT_EQ('A', testFrame.data[4]); // ID Code index 10
		EXPECT_EQ('B', testFrame.data[5]); // ID Code index 11
		EXPECT_EQ('C', testFrame.data[6]); // ID Code index 12
		EXPECT_EQ('*', testFrame.data[7]); // Delimiter

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 3
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x03, testFrame.data[0]); // Sequence 3
		EXPECT_EQ('O', testFrame.data[1]); // Brand index 0
		EXPECT_EQ('p', testFrame.data[2]); // Brand index 1
		EXPECT_EQ('e', testFrame.data[3]); // Brand index 2
		EXPECT_EQ('n', testFrame.data[4]); // Brand index 3
		EXPECT_EQ('-', testFrame.data[5]); // Brand index 4
		EXPECT_EQ('A', testFrame.data[6]); // Brand index 5
		EXPECT_EQ('g', testFrame.data[7]); // Brand index 6

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 4
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x04, testFrame.data[0]); // Sequence 4
		EXPECT_EQ('r', testFrame.data[1]); // Brand index 7
		EXPECT_EQ('i', testFrame.data[2]); // Brand index 8
		EXPECT_EQ('c', testFrame.data[3]); // Brand index 9
		EXPECT_EQ('u', testFrame.data[4]); // Brand index 10
		EXPECT_EQ('l', testFrame.data[5]); // Brand index 11
		EXPECT_EQ('t', testFrame.data[6]); // Brand index 12
		EXPECT_EQ('u', testFrame.data[7]); // Brand index 13

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 5
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x05, testFrame.data[0]); // Sequence 5
		EXPECT_EQ('r', testFrame.data[1]); // Brand index 14
		EXPECT_EQ('e', testFrame.data[2]); // Brand index 15
		EXPECT_EQ('*', testFrame.data[3]); // Delimiter
		EXPECT_EQ('A', testFrame.data[4]); // Model index 0
		EXPECT_EQ('g', testFrame.data[5]); // Model index 1
		EXPECT_EQ('I', testFrame.data[6]); // Model index 2
		EXPECT_EQ('s', testFrame.data[7]); // Model index 3

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 6
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x06, testFrame.data[0]); // Sequence 6
		EXPECT_EQ('o', testFrame.data[1]); // Model index 4
		EXPECT_EQ('S', testFrame.data[2]); // Model index 5
		EXPECT_EQ('t', testFrame.data[3]); // Model index 6
		EXPECT_EQ('a', testFrame.data[4]); // Model index 7
		EXPECT_EQ('c', testFrame.data[5]); // Model index 8
		EXPECT_EQ('k', testFrame.data[6]); // Model index 9
		EXPECT_EQ('+', testFrame.data[7]); // Model index 10

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 7
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x07, testFrame.data[0]); // Sequence 7
		EXPECT_EQ('+', testFrame.data[1]); // Model index 11
		EXPECT_EQ('*', testFrame.data[2]); // Delimiter
		EXPECT_EQ(0xFF, testFrame.data[3]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[4]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[5]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding
	}

	// Make a few test DTCs
	isobus::DiagnosticProtocol::DiagnosticTroubleCode testDTC1(1234, isobus::DiagnosticProtocol::FailureModeIdentifier::ConditionExists, isobus::DiagnosticProtocol::LampStatus::None);
	isobus::DiagnosticProtocol::DiagnosticTroubleCode testDTC2(567, isobus::DiagnosticProtocol::FailureModeIdentifier::DataErratic, isobus::DiagnosticProtocol::LampStatus::AmberWarningLampSlowFlash);
	isobus::DiagnosticProtocol::DiagnosticTroubleCode testDTC3(8910, isobus::DiagnosticProtocol::FailureModeIdentifier::BadIntelligentDevice, isobus::DiagnosticProtocol::LampStatus::RedStopLampSolid);

	{
		// Test DM1
		isobus::DiagnosticProtocol::DiagnosticTroubleCode testDTC1(1234, isobus::DiagnosticProtocol::FailureModeIdentifier::ConditionExists, isobus::DiagnosticProtocol::LampStatus::None);
		isobus::DiagnosticProtocol::DiagnosticTroubleCode testDTC2(567, isobus::DiagnosticProtocol::FailureModeIdentifier::DataErratic, isobus::DiagnosticProtocol::LampStatus::AmberWarningLampSlowFlash);
		isobus::DiagnosticProtocol::DiagnosticTroubleCode testDTC3(8910, isobus::DiagnosticProtocol::FailureModeIdentifier::BadIntelligentDevice, isobus::DiagnosticProtocol::LampStatus::RedStopLampSolid);

		protocolUnderTest.set_diagnostic_trouble_code_active(testDTC1, true);

		// Use a PGN request to trigger sending it immediately
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xCA;
		testFrame.data[1] = 0xFE;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();

		protocolUnderTest.update();

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// A single DTC is 1 frame
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18FECAAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0xFF, testFrame.data[0]); // Lamp (unused in ISO11783 mode)
		EXPECT_EQ(0xFF, testFrame.data[1]); // Lamp (unused in ISO11783 mode)
		EXPECT_EQ(0xD2, testFrame.data[2]); // SPN LSB
		EXPECT_EQ(0x04, testFrame.data[3]); // SPN
		EXPECT_EQ(31, testFrame.data[4]); // SPN + FMI
		EXPECT_EQ(1, testFrame.data[5]); // Occurrence Count  + Conversion Method
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding

		protocolUnderTest.set_j1939_mode(true);
		EXPECT_TRUE(protocolUnderTest.get_j1939_mode());

		// Validate in J1939 mode
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xCA;
		testFrame.data[1] = 0xFE;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18FECAAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x00, testFrame.data[0]); // Lamp
		EXPECT_EQ(0xFF, testFrame.data[1]); // Flash (do not flash / solid)
		EXPECT_EQ(0xD2, testFrame.data[2]); // SPN LSB
		EXPECT_EQ(0x04, testFrame.data[3]); // SPN
		EXPECT_EQ(31, testFrame.data[4]); // SPN + FMI
		EXPECT_EQ(1, testFrame.data[5]); // Occurrence Count  + Conversion Method
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding

		protocolUnderTest.set_j1939_mode(false);
		EXPECT_FALSE(protocolUnderTest.get_j1939_mode());

		// Test a DM1 with multiple DTCs in it
		protocolUnderTest.set_diagnostic_trouble_code_active(testDTC2, true);
		protocolUnderTest.set_diagnostic_trouble_code_active(testDTC3, true);
		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xCA;
		testFrame.data[1] = 0xFE;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();

		// Wait for BAM
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		std::uint16_t expectedBAMLength = 14; // This is 2 + 4 * number of DTCs

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// Broadcast Announce Message
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18ECFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x20, testFrame.data[0]); // BAM Multiplexer
		EXPECT_EQ(expectedBAMLength & 0xFF, testFrame.data[1]); // Length LSB
		EXPECT_EQ((expectedBAMLength >> 8) & 0xFF, testFrame.data[2]); // Length MSB
		EXPECT_EQ(0x02, testFrame.data[3]); // Number of frames in session (based on length)
		EXPECT_EQ(0xFF, testFrame.data[4]); // Always 0xFF
		EXPECT_EQ(0xCA, testFrame.data[5]); // PGN LSB
		EXPECT_EQ(0xFE, testFrame.data[6]); // PGN
		EXPECT_EQ(0x00, testFrame.data[7]); // PGN MSB

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 1
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // Sequence 1
		EXPECT_EQ(0xFF, testFrame.data[1]); // Lamp / reserved
		EXPECT_EQ(0xFF, testFrame.data[2]); // Flash / reserved
		EXPECT_EQ(0xD2, testFrame.data[3]); // SPN 1
		EXPECT_EQ(0x04, testFrame.data[4]); // SPN 1
		EXPECT_EQ(31, testFrame.data[5]); // FMI 1
		EXPECT_EQ(1, testFrame.data[6]); // Count 1
		EXPECT_EQ(0x37, testFrame.data[7]); // SPN2

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 2
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x02, testFrame.data[0]); // Sequence 2
		EXPECT_EQ(0x02, testFrame.data[1]); // SPN 2
		EXPECT_EQ(2, testFrame.data[2]); // FMI 2
		EXPECT_EQ(01, testFrame.data[3]); // Count 2
		EXPECT_EQ(0xCE, testFrame.data[4]); // SPN 3
		EXPECT_EQ(0x22, testFrame.data[5]); // SPN 3
		EXPECT_EQ(12, testFrame.data[6]); // FMI 3
		EXPECT_EQ(1, testFrame.data[7]); // Count 3
	}

	{
		// Test DM2
		protocolUnderTest.set_diagnostic_trouble_code_active(testDTC1, false);
		protocolUnderTest.set_diagnostic_trouble_code_active(testDTC2, false);
		protocolUnderTest.set_diagnostic_trouble_code_active(testDTC3, false);

		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xCB;
		testFrame.data[1] = 0xFE;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();

		// Wait for BAM
		std::this_thread::sleep_for(std::chrono::milliseconds(250));

		EXPECT_TRUE(testPlugin.read_frame(testFrame));
		std::uint16_t expectedBAMLength = 14; // This is 2 + 4 * number of DTCs

		// Broadcast Announce Message
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18ECFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x20, testFrame.data[0]); // BAM Multiplexer
		EXPECT_EQ(expectedBAMLength & 0xFF, testFrame.data[1]); // Length LSB
		EXPECT_EQ((expectedBAMLength >> 8) & 0xFF, testFrame.data[2]); // Length MSB
		EXPECT_EQ(0x02, testFrame.data[3]); // Number of frames in session (based on length)
		EXPECT_EQ(0xFF, testFrame.data[4]); // Always 0xFF
		EXPECT_EQ(0xCB, testFrame.data[5]); // PGN LSB
		EXPECT_EQ(0xFE, testFrame.data[6]); // PGN
		EXPECT_EQ(0x00, testFrame.data[7]); // PGN MSB

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 1
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x01, testFrame.data[0]); // Sequence 1
		EXPECT_EQ(0xFF, testFrame.data[1]); // Lamp / reserved
		EXPECT_EQ(0xFF, testFrame.data[2]); // Flash / reserved
		EXPECT_EQ(0xD2, testFrame.data[3]); // SPN 1
		EXPECT_EQ(0x04, testFrame.data[4]); // SPN 1
		EXPECT_EQ(31, testFrame.data[5]); // FMI 1
		EXPECT_EQ(1, testFrame.data[6]); // Count 1
		EXPECT_EQ(0x37, testFrame.data[7]); // SPN2

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// BAM Payload Frame 2
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x1CEBFFAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0x02, testFrame.data[0]); // Sequence 2
		EXPECT_EQ(0x02, testFrame.data[1]); // SPN 2
		EXPECT_EQ(2, testFrame.data[2]); // FMI 2
		EXPECT_EQ(01, testFrame.data[3]); // Count 2
		EXPECT_EQ(0xCE, testFrame.data[4]); // SPN 3
		EXPECT_EQ(0x22, testFrame.data[5]); // SPN 3
		EXPECT_EQ(12, testFrame.data[6]); // FMI 3
		EXPECT_EQ(1, testFrame.data[7]); // Count 3

		// Clear the DTCs
		protocolUnderTest.clear_inactive_diagnostic_trouble_codes();

		testFrame.dataLength = 3;
		testFrame.identifier = 0x18EAAAAB;
		testFrame.data[0] = 0xCB;
		testFrame.data[1] = 0xFE;
		testFrame.data[2] = 0x00;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// Now zero DTCs
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18FECBAA, testFrame.identifier); // BAM from address AA
		EXPECT_EQ(0xFF, testFrame.data[0]); // Lamp (unused in ISO11783 mode)
		EXPECT_EQ(0xFF, testFrame.data[1]); // Lamp (unused in ISO11783 mode)
		EXPECT_EQ(0x00, testFrame.data[2]); // SPN LSB
		EXPECT_EQ(0x00, testFrame.data[3]); // SPN
		EXPECT_EQ(0x00, testFrame.data[4]); // SPN + FMI
		EXPECT_EQ(0x00, testFrame.data[5]); // Occurrence Count  + Conversion Method
		EXPECT_EQ(0xFF, testFrame.data[6]); // Padding
		EXPECT_EQ(0xFF, testFrame.data[7]); // Padding
	}

	{
		// Test DM13 against J1939-73
		EXPECT_TRUE(protocolUnderTest.get_broadcast_state());
		EXPECT_TRUE(protocolUnderTest.suspend_broadcasts(5));

		EXPECT_TRUE(testPlugin.read_frame(testFrame));

		// When we are announcing a suspension, we're supposed to set
		// all values to NA except for the time, which we set to 5 in this case
		EXPECT_EQ(CAN_DATA_LENGTH, testFrame.dataLength);
		EXPECT_EQ(0x18DFFFAA, testFrame.identifier); // DM13 from address AA
		EXPECT_EQ(0xFF, testFrame.data[0]);
		EXPECT_EQ(0xFF, testFrame.data[1]);
		EXPECT_EQ(0xFF, testFrame.data[2]);
		EXPECT_EQ(0xFF, testFrame.data[3]);
		EXPECT_EQ(0x05, testFrame.data[4]);
		EXPECT_EQ(0x00, testFrame.data[5]);
		EXPECT_EQ(0xFF, testFrame.data[6]);
		EXPECT_EQ(0xFF, testFrame.data[7]);

		EXPECT_FALSE(protocolUnderTest.get_broadcast_state());

		// Wait suspension to be lifted
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		protocolUnderTest.update();
		EXPECT_TRUE(protocolUnderTest.get_broadcast_state());

		// Test a suspension by another ECU. Set only our network.
		testFrame.dataLength = 8;
		testFrame.data[0] = 0xFC;
		testFrame.data[1] = 0xFF;
		testFrame.data[2] = 0xFF;
		testFrame.data[3] = 0x00;
		testFrame.data[4] = 0x0A;
		testFrame.data[5] = 0x00;
		testFrame.data[6] = 0xFF;
		testFrame.data[7] = 0xFF;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();
		EXPECT_FALSE(protocolUnderTest.get_broadcast_state());

		// Restart broadcasts
		testFrame.dataLength = 8;
		testFrame.data[0] = 0xFD;
		testFrame.data[1] = 0xFF;
		testFrame.data[2] = 0xFF;
		testFrame.data[3] = 0x00;
		testFrame.data[4] = 0xFF;
		testFrame.data[5] = 0xFF;
		testFrame.data[6] = 0xFF;
		testFrame.data[7] = 0xFF;
		CANNetworkManager::process_receive_can_message_frame(testFrame);
		CANNetworkManager::CANNetwork.update();
		protocolUnderTest.update();
		EXPECT_TRUE(protocolUnderTest.get_broadcast_state());
	}
	protocolUnderTest.terminate();
	EXPECT_FALSE(protocolUnderTest.get_initialized());
	CANHardwareInterface::stop();
}
