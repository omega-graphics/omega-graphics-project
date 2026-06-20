#include "omega-common/json.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace OmegaCommon {

class RapidJSONBridge {
  using Document = rapidjson::Document;
  using Value = rapidjson::Value;

  [[noreturn]] static void fail(const String &message) {
    std::cerr << "JSON Parse Error:" << message << std::endl;
    std::exit(1);
  }

  template <typename Writer>
  static void writeNode(JSON &json, Writer &writer) {
    if (json.type == JSON::MAP) {
      writer.StartObject();
      auto map = json.asMap();
      for (auto it = map.begin(); it != map.end(); ++it) {
        writer.Key(it->first.c_str(),
                   static_cast<rapidjson::SizeType>(it->first.size()));
        writeNode(const_cast<JSON &>(it->second), writer);
      }
      writer.EndObject();
      return;
    }

    if (json.type == JSON::ARRAY) {
      writer.StartArray();
      auto vec = json.asVector();
      for (auto it = vec.begin(); it != vec.end(); ++it) {
        writeNode(const_cast<JSON &>(*it), writer);
      }
      writer.EndArray();
      return;
    }

    if (json.type == JSON::STRING) {
      auto str = json.asString();
      writer.String(str.data(), static_cast<rapidjson::SizeType>(str.size()));
      return;
    }

    if (json.type == JSON::NUMBER) {
      writer.Int(json.data.number);
      return;
    }

    if (json.type == JSON::BOOLEAN) {
      writer.Bool(json.data.b);
      return;
    }

    writer.Null();
  }

public:
  static JSON fromRapid(const Value &value) {
    if (value.IsObject()) {
      JSON json {};
      json.type = JSON::MAP;
      json.data = JSON::Data(JSON::MAP);
      json.data.map = new Map<String, JSON>();
      for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        String key(it->name.GetString(), it->name.GetStringLength());
        json.data.map->insert(std::make_pair(std::move(key), fromRapid(it->value)));
      }
      return json;
    }

    if (value.IsArray()) {
      JSON json {};
      json.type = JSON::ARRAY;
      json.data = JSON::Data(JSON::ARRAY);
      json.data.array = new Vector<JSON>();
      json.data.array->reserve(value.Size());
      for (auto it = value.Begin(); it != value.End(); ++it) {
        json.data.array->push_back(fromRapid(*it));
      }
      return json;
    }

    if (value.IsString()) {
      return JSON(String(value.GetString(), value.GetStringLength()));
    }

    if (value.IsBool()) {
      return JSON(value.GetBool());
    }

    if (value.IsNumber()) {
      JSON json {};
      json.type = JSON::NUMBER;
      if (value.IsInt()) {
        json.data.number = value.GetInt();
      } else if (value.IsUint()) {
        json.data.number = static_cast<int>(value.GetUint());
      } else if (value.IsInt64()) {
        json.data.number = static_cast<int>(value.GetInt64());
      } else if (value.IsUint64()) {
        json.data.number = static_cast<int>(value.GetUint64());
      } else {
        json.data.number = static_cast<int>(value.GetDouble());
      }
      return json;
    }

    if (value.IsNull()) {
      return JSON {};
    }

    fail("Unsupported RapidJSON value type");
  }

  static JSON parse(const String &source) {
    Document document;
    document.Parse(source.c_str(), source.size());
    if (document.HasParseError()) {
      String message =
          String(rapidjson::GetParseError_En(document.GetParseError())) + " at offset " +
          std::to_string(document.GetErrorOffset());
      fail(message);
    }
    return fromRapid(document);
  }

  static JSON parse(std::istream &in) {
    rapidjson::IStreamWrapper wrapper(in);
    Document document;
    document.ParseStream(wrapper);
    if (document.HasParseError()) {
      String message =
          String(rapidjson::GetParseError_En(document.GetParseError())) + " at offset " +
          std::to_string(document.GetErrorOffset());
      fail(message);
    }
    return fromRapid(document);
  }

  static void serialize(JSON &json, std::ostream &out) {
    rapidjson::OStreamWrapper wrapper(out);
    rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(wrapper);
    writeNode(json, writer);
  }
};

bool JSON::isString() const {
  return type == STRING;
}

bool JSON::isArray() const {
  return type == ARRAY;
}

bool JSON::isMap() const {
  return type == MAP;
}

bool JSON::isNumber() const {
  return type == NUMBER;
}

MapRef<String, JSON> JSON::asMap() {
  assert(isMap());
  return *data.map;
}

ArrayRef<JSON> JSON::asVector() {
  assert(isArray());
  return *data.array;
}

StrRef JSON::asString() {
  assert(isString());
  return {data.str};
}

float JSON::asFloat() {
  assert(isNumber());
  return static_cast<float>(data.number);
}

bool &JSON::asBool() {
  assert(type == BOOLEAN);
  return data.b;
}

JSON JSON::parse(String str) {
  return RapidJSONBridge::parse(str);
}

JSON JSON::parse(std::istream &in) {
  return RapidJSONBridge::parse(in);
}

String JSON::serialize(JSON &json) {
  std::ostringstream out;
  serialize(json, out);
  return out.str();
}

void JSON::serialize(JSON &json, std::ostream &out) {
  RapidJSONBridge::serialize(json, out);
}

JSON::Data::Data(decltype(JSON::type) t) : Data() {
  if (t == ARRAY) {
    array = nullptr;
  } else if (t == MAP) {
    map = nullptr;
  }
}

JSON::Data::Data(StrRef str) : str(new char[str.size() + 1]) {
  std::move(str.begin(), str.end(), this->str);
  this->str[str.size()] = '\0';
}

JSON::Data::Data(ArrayRef<JSON> array)
    : array(new Vector<JSON>(array.begin(), array.end())) {}

JSON::Data::Data(MapRef<String, JSON> map)
    : map(new Map<String, JSON>(map.begin(), map.end())) {}

JSON::Data::Data(bool &b) : b(b) {}

void JSON::Data::_destroy(decltype(type) t) {
  if (t == STRING) {
    delete[] str;
  } else if (t == MAP) {
    delete map;
  } else if (t == ARRAY) {
    delete array;
  }
}

JSON::JSON(const JSON &other) : type(other.type) {
  switch (other.type) {
  case STRING:
    // Data(StrRef) allocates a fresh, null-terminated buffer and copies into it.
    data = Data(StrRef(other.data.str));
    break;
  case ARRAY:
    // Vector's copy constructor deep-copies each element via this same ctor.
    data.array = other.data.array ? new Vector<JSON>(*other.data.array)
                                  : new Vector<JSON>();
    break;
  case MAP:
    data.map = other.data.map ? new Map<String, JSON>(*other.data.map)
                              : new Map<String, JSON>();
    break;
  case NUMBER:
    data.number = other.data.number;
    break;
  case BOOLEAN:
    data.b = other.data.b;
    break;
  case UNKNOWN:
  default:
    break;
  }
}

JSON::JSON(JSON &&other) noexcept : type(other.type), data(other.data) {
  // Steal other's storage, then leave it an empty node so its destructor frees
  // nothing (the buffers are now owned by *this).
  other.type = UNKNOWN;
}

JSON &JSON::operator=(const JSON &other) {
  if (this != &other) {
    // Clone first (the only step that can throw) so a failure leaves *this
    // untouched, then release our old storage and adopt the clone's.
    JSON copy(other);
    data._destroy(type);
    type = copy.type;
    data = copy.data;
    copy.type = UNKNOWN;
  }
  return *this;
}

JSON &JSON::operator=(JSON &&other) noexcept {
  if (this != &other) {
    data._destroy(type);
    type = other.type;
    data = other.data;
    other.type = UNKNOWN;
  }
  return *this;
}

JSON::~JSON() { data._destroy(type); }

JSON::JSON(const char *c_str) : type(STRING), data(c_str) {}

JSON::JSON(const String &str) : type(STRING), data(str) {}

JSON::JSON(bool b) : type(BOOLEAN), data(b) {}

JSON::JSON(std::initializer_list<JSON> array)
    : type(ARRAY),
      data(ArrayRef<JSON>{const_cast<JSON *>(array.begin()),
                          const_cast<JSON *>(array.end())}) {}

JSON &JSON::operator[](OmegaCommon::StrRef str) {
  assert(isMap() && "Cannot insert pair unless object is Map");
  return data.map->operator[](str);
}

JSON::map_iterator JSON::insert(const std::pair<String, JSON> &j) {
  assert(isMap() && "Cannot insert pair unless object is Map");
  return data.map->insert(j).first;
}

void JSON::push_back(const JSON &j) {
  assert(isArray() && "Cannot push object unless is Array");
  data.array->push_back(j);
}

JSON::JSON(std::map<String, JSON> map) : type(MAP), data(map) {}

std::istream &operator>>(std::istream &in, JSON &json) {
  json = JSON::parse(in);
  return in;
}

std::ostream &operator<<(std::ostream &out, JSON &json) {
  JSON::serialize(json, out);
  return out;
}

} // namespace OmegaCommon
