#include "regmap.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>

#if (__GNUC__ < 8)
#include <experimental/filesystem>
namespace std {
namespace filesystem = experimental::filesystem;
}
#else
#include <filesystem>
#endif

using nlohmann::json;

static void streamHex(std::ostream& os, size_t num, size_t ndigits) {
  os << std::hex << std::setfill('0') << std::setw(ndigits) << std::right
     << num;
}

bool AddrRange::contains(uint8_t addr) const {
  return addr >= range.first && addr <= range.second;
}

void from_json(const json& j, AddrRange& a) {
  a.range = j;
}
void to_json(json& j, const AddrRange& a) {
  j = a.range;
}

NLOHMANN_JSON_SERIALIZE_ENUM(
    RegisterValueType,
    {
        {RegisterValueType::HEX, "hex"},
        {RegisterValueType::STRING, "string"},
        {RegisterValueType::INTEGER, "integer"},
        {RegisterValueType::FLOAT, "float"},
        {RegisterValueType::FLAGS, "flags"},
    })

void from_json(const json& j, RegisterDescriptor& i) {
  j.at("begin").get_to(i.begin);
  j.at("length").get_to(i.length);
  j.at("name").get_to(i.name);
  i.keep = j.value("keep", 1);
  i.storeChangesOnly = j.value("changes_only", false);
  i.format = j.value("format", RegisterValueType::HEX);
  if (i.format == RegisterValueType::FLOAT) {
    j.at("precision").get_to(i.precision);
  } else if (i.format == RegisterValueType::FLAGS) {
    j.at("flags").get_to(i.flags);
  }
}
void to_json(json& j, const RegisterDescriptor& i) {
  j["begin"] = i.begin;
  j["length"] = i.length;
  j["name"] = i.name;
  j["keep"] = i.keep;
  j["changes_only"] = i.storeChangesOnly;
  j["format"] = i.format;
  if (i.format == RegisterValueType::FLOAT) {
    j["precision"] = i.precision;
  } else if (i.format == RegisterValueType::FLAGS) {
    j["flags"] = i.flags;
  }
}

void RegisterValue::makeString(const std::vector<uint16_t>& reg) {
  // Manual construction of string (non-trivial member of union)
  new (&value.strValue)(std::string);
  // String is stored normally H L H L, so we
  // need reswap the bytes in each nibble.
  for (const auto& r : reg) {
    char ch = r >> 8;
    char cl = r & 0xff;
    if (ch == '\0')
      break;
    value.strValue += ch;
    if (cl == '\0')
      break;
    value.strValue += cl;
  }
}

void RegisterValue::makeHex(const std::vector<uint16_t>& reg) {
  // Manual construction of vector (non-trivial member of union)
  new (&value.hexValue)(std::vector<uint8_t>);
  for (uint16_t v : reg) {
    value.hexValue.push_back(v >> 8);
    value.hexValue.push_back(v & 0xff);
  }
}

void RegisterValue::makeInteger(const std::vector<uint16_t>& reg) {
  // TODO We currently do not need more than 32bit values as per
  // our current/planned regmaps. If such a value should show up in the
  // future, then we might need to return std::variant<int32_t,int64_t>.
  if (reg.size() > 2)
    throw std::out_of_range("Register does not fit as an integer");
  // Everything in modbus is Big-endian. So when we have a list
  // of registers forming a larger value; For example,
  // a 32bit value would be 2 16bit regs.
  // Then the first register would be the upper nibble of the
  // resulting 32bit value.
  value.intValue =
      std::accumulate(reg.begin(), reg.end(), 0, [](int32_t ac, uint16_t v) {
        return (ac << 16) + v;
      });
}

void RegisterValue::makeFloat(
    const std::vector<uint16_t>& reg,
    uint16_t precision) {
  makeInteger(reg);
  // Y = X / 2^N
  value.floatValue = float(value.intValue) / float(1 << precision);
}

void RegisterValue::makeFlags(
    const std::vector<uint16_t>& reg,
    const RegisterDescriptor::FlagsDescType& flagsDesc) {
  makeInteger(reg);
  uint32_t bitField = static_cast<uint32_t>(value.intValue);
  new (&value.flagsValue)(FlagsType);
  for (const auto& [pos, name] : flagsDesc) {
    bool bitVal = (bitField & (1 << pos)) != 0;
    value.flagsValue.push_back(std::make_tuple(bitVal, name));
  }
}

RegisterValue::RegisterValue(
    const std::vector<uint16_t>& reg,
    const RegisterDescriptor& desc,
    uint32_t tstamp)
    : type(desc.format), timestamp(tstamp) {
  switch (desc.format) {
    case RegisterValueType::STRING:
      makeString(reg);
      break;
    case RegisterValueType::INTEGER:
      makeInteger(reg);
      break;
    case RegisterValueType::FLOAT:
      makeFloat(reg, desc.precision);
      break;
    case RegisterValueType::FLAGS:
      makeFlags(reg, desc.flags);
      break;
    case RegisterValueType::HEX:
      makeHex(reg);
  }
}

RegisterValue::RegisterValue(const std::vector<uint16_t>& reg)
    : type(RegisterValueType::HEX) {
  makeHex(reg);
}

RegisterValue::RegisterValue(const RegisterValue& other)
    : type(other.type), timestamp(other.timestamp) {
  switch (type) {
    case RegisterValueType::HEX:
      new (&value.hexValue) auto(other.value.hexValue);
      break;
    case RegisterValueType::INTEGER:
      value.intValue = other.value.intValue;
      break;
    case RegisterValueType::FLOAT:
      value.floatValue = other.value.floatValue;
      break;
    case RegisterValueType::STRING:
      new (&value.strValue) auto(other.value.strValue);
      break;
    case RegisterValueType::FLAGS:
      new (&value.flagsValue) auto(other.value.flagsValue);
      break;
  }
}

RegisterValue::RegisterValue(RegisterValue&& other)
    : type(other.type), timestamp(other.timestamp) {
  switch (type) {
    case RegisterValueType::HEX:
      new (&value.hexValue) auto(std::move(other.value.hexValue));
      break;
    case RegisterValueType::INTEGER:
      value.intValue = other.value.intValue;
      break;
    case RegisterValueType::FLOAT:
      value.floatValue = other.value.floatValue;
      break;
    case RegisterValueType::STRING:
      new (&value.strValue) auto(std::move(other.value.strValue));
      break;
    case RegisterValueType::FLAGS:
      new (&value.flagsValue) auto(std::move(other.value.flagsValue));
      break;
  }
}

RegisterValue::~RegisterValue() {
  switch (type) {
    case RegisterValueType::HEX:
      value.hexValue.~vector<uint8_t>();
      break;
    case RegisterValueType::STRING:
      value.strValue.~basic_string();
      break;
    case RegisterValueType::FLAGS:
      value.flagsValue.~FlagsType();
      break;
    case RegisterValueType::INTEGER:
      [[fallthrough]];
    case RegisterValueType::FLOAT:
      // Do nothing
      break;
  }
}

RegisterValue::operator std::string() {
  std::string ret = "";
  std::stringstream os;
  switch (type) {
    case RegisterValueType::STRING:
      os << value.strValue;
      break;
    case RegisterValueType::INTEGER:
      os << value.intValue;
      break;
    case RegisterValueType::FLOAT:
      os << std::fixed << std::setprecision(2) << value.floatValue;
      break;
    case RegisterValueType::FLAGS:
      // We could technically be clever and pack this as a
      // JSON object. But considering this is designed for
      // human consumption only, we can make it pretty
      // (and backwards compatible with V1's output).
      for (auto& [bitval, name] : value.flagsValue) {
        if (bitval)
          os << "\n*[1] ";
        else
          os << "\n [0] ";
        os << name;
      }
      break;
    case RegisterValueType::HEX:
      for (uint8_t byte : value.hexValue) {
        os << std::hex << std::setw(2) << std::setfill('0') << int(byte);
      }
      break;
    default:
      throw std::runtime_error("Unknown format type: " + std::to_string(type));
  }
  return os.str();
}

void to_json(json& j, const RegisterValue& m) {
  j["type"] = m.type;
  j["time"] = m.timestamp;
  switch (m.type) {
    case RegisterValueType::HEX:
      j["value"] = m.value.hexValue;
      break;
    case RegisterValueType::STRING:
      j["value"] = m.value.strValue;
      break;
    case RegisterValueType::INTEGER:
      j["value"] = m.value.intValue;
      break;
    case RegisterValueType::FLOAT:
      j["value"] = m.value.floatValue;
      break;
    case RegisterValueType::FLAGS:
      j["value"] = m.value.flagsValue;
      break;
  }
}

Register::operator std::string() const {
  return RegisterValue(value, desc, timestamp);
}

Register::operator RegisterValue() const {
  return RegisterValue(value, desc, timestamp);
}

void to_json(json& j, const Register& m) {
  j["time"] = m.timestamp;
  std::string data = RegisterValue(m.value);
  j["data"] = data;
}

RegisterStore::operator std::string() const {
  std::stringstream ss;

  // Format we are going for.
  // "  <0x0000> MFG_MODEL                        :700-014671-0000  "
  ss << "  <0x";
  streamHex(ss, desc_.begin, 4);
  ss << "> " << std::setfill(' ') << std::setw(32) << std::left << desc_.name
     << " :";
  for (const auto& v : history_) {
    if (v) {
      if (desc_.format != RegisterValueType::FLAGS)
        ss << ' ';
      else
        ss << '\n';
      ss << std::string(v);
    }
  }
  return ss.str();
}

RegisterStore::operator RegisterStoreValue() const {
  RegisterStoreValue ret(regAddr_, desc_.name);
  for (const auto& reg : history_) {
    if (reg)
      ret.history.emplace_back(reg);
  }
  return ret;
}

void to_json(json& j, const RegisterStoreValue& m) {
  j["regAddress"] = m.regAddr;
  j["name"] = m.name;
  j["readings"] = m.history;
}

void to_json(json& j, const RegisterStore& m) {
  j["begin"] = m.regAddr_;
  j["readings"] = m.history_;
}

void from_json(const json& j, WriteActionInfo& action) {
  j.at("interpret").get_to(action.interpret);
  if (j.contains("shell"))
    action.shell = j.at("shell");
  else
    action.shell = std::nullopt;
  if (j.contains("value"))
    action.value = j.at("value");
  else
    action.value = std::nullopt;
  if (!action.shell && !action.value)
    throw std::runtime_error("Bad special handler");
}

void from_json(const json& j, SpecialHandlerInfo& m) {
  j.at("reg").get_to(m.reg);
  j.at("len").get_to(m.len);
  m.period = j.value("period", -1);
  j.at("action").get_to(m.action);
  if (m.action != "write")
    throw std::runtime_error("Unsupported action: " + m.action);
  j.at("info").get_to(m.info);
}

void from_json(const json& j, RegisterMap& m) {
  j.at("address_range").get_to(m.applicableAddresses);
  j.at("probe_register").get_to(m.probeRegister);
  j.at("name").get_to(m.name);
  j.at("preferred_baudrate").get_to(m.preferredBaudrate);
  j.at("default_baudrate").get_to(m.defaultBaudrate);
  std::vector<RegisterDescriptor> tmp;
  j.at("registers").get_to(tmp);
  for (auto& i : tmp) {
    m.registerDescriptors[i.begin] = i;
  }
  if (j.contains("special_handlers")) {
    j.at("special_handlers").get_to(m.specialHandlers);
  }
}
void to_json(json& j, const RegisterMap& m) {
  j["address_range"] = m.applicableAddresses;
  j["probe_register"] = m.probeRegister;
  j["name"] = m.name;
  j["preferred_baudrate"] = m.preferredBaudrate;
  j["default_baudrate"] = m.preferredBaudrate;
  j["registers"] = {};
  std::transform(
      m.registerDescriptors.begin(),
      m.registerDescriptors.end(),
      std::back_inserter(j["registers"]),
      [](const auto& kv) { return kv.second; });
}

const RegisterMap& RegisterMapDatabase::at(uint8_t addr) {
  const auto& result = find_if(
      regmaps.begin(),
      regmaps.end(),
      [addr](const std::unique_ptr<RegisterMap>& m) {
        return m->applicableAddresses.contains(addr);
      });
  if (result == regmaps.end())
    throw std::out_of_range("not found: " + std::to_string(int(addr)));
  return **result;
}

void RegisterMapDatabase::load(const nlohmann::json& j) {
  std::unique_ptr<RegisterMap> rmap = std::make_unique<RegisterMap>();
  *rmap = j;
  regmaps.push_back(std::move(rmap));
}

void RegisterMapDatabase::load(const std::string& dir) {
  for (auto const& dir_entry : std::filesystem::directory_iterator{dir}) {
    std::ifstream ifs(dir_entry.path().string());
    json j;
    ifs >> j;
    load(j);
    ifs.close();
  }
}

void RegisterMapDatabase::print(std::ostream& os) {
  json j = {};
  std::transform(
      regmaps.begin(),
      regmaps.end(),
      std::back_inserter(j),
      [](const auto& ptr) { return *ptr; });
  os << j.dump(4);
}
