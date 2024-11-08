//================================================================================================
/// @file ntcan_plugin.hpp
///
/// @brief An interface for using a ESD driver.
/// @attention Use of the NTCAN driver is governed in part by their license, and requires you
/// to install their driver first, which in-turn requires you to agree to their terms and conditions.
/// @author Alex "Y_Less" Cole
/// @author Daan Steenbergen
///
/// @copyright 2024 The Open-Agriculture Developers
//================================================================================================
#ifndef NTCAN_PLUGIN_HPP
#define NTCAN_PLUGIN_HPP

#include <string>
#include "ntcan.h"

#include "isobus/hardware_integration/can_hardware_plugin.hpp"
#include "isobus/isobus/can_hardware_abstraction.hpp"
#include "isobus/isobus/can_message_frame.hpp"

namespace isobus
{
	/// @brief A CAN Driver for ESD NTCAN Devices
	class NTCANPlugin : public CANHardwarePlugin
	{
	public:
		/// @brief Constructor for the ESD NTCAN CAN driver
		/// @param[in] channel The logical net number assigned to the physical CAN port to use.
		explicit NTCANPlugin(int channel);

		/// @brief The destructor for NTCANPlugin
		virtual ~NTCANPlugin() = default;

		/// @brief Returns if the connection with the hardware is valid
		/// @returns `true` if connected, `false` if not connected
		bool get_is_valid() const override;

		/// @brief Closes the connection to the hardware
		void close() override;

		/// @brief Connects to the hardware you specified in the constructor's channel argument
		void open() override;

		/// @brief Returns a frame from the hardware (synchronous), or `false` if no frame can be read.
		/// @param[in, out] canFrame The CAN frame that was read
		/// @returns `true` if a CAN frame was read, otherwise `false`
		bool read_frame(isobus::CANMessageFrame &canFrame) override;

		/// @brief Writes a frame to the bus (synchronous)
		/// @param[in] canFrame The frame to write to the bus
		/// @returns `true` if the frame was written, otherwise `false`
		bool write_frame(const isobus::CANMessageFrame &canFrame) override;

	private:
		int net;
		std::uint64_t timestampFreq;
		std::uint64_t timestampOff;
		NTCAN_HANDLE handle; ///< The handle as defined in the NTCAN driver API
		NTCAN_RESULT openResult; ///< Stores the result of the call to begin CAN communication. Used for is_valid check later.
	};
}

#endif // NTCAN_PLUGIN_HPP
