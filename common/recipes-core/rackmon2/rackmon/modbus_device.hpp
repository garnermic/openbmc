#include <nlohmann/json.hpp>
#include <ctime>
#include <iostream>
#include "modbus.hpp"
#include "modbus_cmds.hpp"
#include "regmap.hpp"

enum ModbusDeviceMode { ACTIVE = 0, DORMANT = 1 };

class ModbusDevice;

class ModbusSpecialHandler : public SpecialHandlerInfo {
  time_t lastHandleTime_ = 0;
  bool handled_ = false;
  bool canHandle() {
    if (period == -1)
      return !handled_;
    return std::time(0) > (lastHandleTime_ + period);
  }

 public:
  void handle(ModbusDevice& dev);
};

// Generic Device information
struct ModbusDeviceInfo {
  static constexpr uint32_t kMaxConsecutiveFailures = 10;
  uint8_t deviceAddress = 0;
  std::string deviceType{"Unknown"};
  uint32_t baudrate = 0;
  ModbusDeviceMode mode = ModbusDeviceMode::ACTIVE;
  uint32_t crcErrors = 0;
  uint32_t timeouts = 0;
  uint32_t miscErrors = 0;
  time_t lastActive = 0;
  uint32_t numConsecutiveFailures = 0;

  void incErrors(uint32_t& counter);
  void incTimeouts() {
    incErrors(timeouts);
  }
  void incCRCErrors() {
    incErrors(crcErrors);
  }
  void incMiscErrors() {
    incErrors(miscErrors);
  }
};
void to_json(nlohmann::json& j, const ModbusDeviceInfo& m);

// Device raw register data
struct ModbusDeviceRawData : public ModbusDeviceInfo {
  std::vector<RegisterStore> registerList{};
};
void to_json(nlohmann::json& j, const ModbusDeviceRawData& m);

// Device string data format (deprecated)
struct ModbusDeviceFmtData : public ModbusDeviceInfo {
  std::vector<std::string> registerList{};
};
void to_json(nlohmann::json& j, const ModbusDeviceFmtData& m);

// Device interpreted register value format
struct ModbusDeviceValueData : public ModbusDeviceInfo {
  std::vector<RegisterStoreValue> registerList{};
};
void to_json(nlohmann::json& j, const ModbusDeviceValueData& m);

class ModbusDevice {
  Modbus& interface_;
  ModbusDeviceRawData info_;
  const RegisterMap& registerMap_;
  std::mutex registerListMutex_{};
  std::vector<ModbusSpecialHandler> specialHandlers_{};

 public:
  ModbusDevice(
      Modbus& interface,
      uint8_t deviceAddress,
      const RegisterMap& registerMap);
  virtual ~ModbusDevice() {}

  virtual void command(
      Msg& req,
      Msg& resp,
      ModbusTime timeout = ModbusTime::zero(),
      ModbusTime settleTime = ModbusTime::zero());

  void readHoldingRegisters(
      uint16_t registerOffset,
      std::vector<uint16_t>& regs,
      ModbusTime timeout = ModbusTime::zero());

  void writeSingleRegister(
      uint16_t registerOffset,
      uint16_t value,
      ModbusTime timeout = ModbusTime::zero());

  void writeMultipleRegisters(
      uint16_t registerOffset,
      std::vector<uint16_t>& value,
      ModbusTime timeout = ModbusTime::zero());

  void readFileRecord(
      std::vector<FileRecord>& records,
      ModbusTime timeout = ModbusTime::zero());

  void monitor();

  bool isActive() const {
    return info_.mode == ModbusDeviceMode::ACTIVE;
  }
  void setActive() {
    info_.numConsecutiveFailures = 0;
  }
  time_t lastActive() const {
    return info_.lastActive;
  }

  // Return structured information of the device.
  ModbusDeviceInfo getInfo();

  // Returns raw monitor register data monitored for this device.
  ModbusDeviceRawData getRawData();

  // (deprecated) Returns string formatted register data
  ModbusDeviceFmtData getFmtData();

  // Returns value formatted register data monitored for this device.
  ModbusDeviceValueData getValueData();

  // Allow special handler access into the device.
  friend ModbusSpecialHandler;
};
