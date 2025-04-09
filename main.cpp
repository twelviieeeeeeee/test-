#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <type_traits>
#include <variant>
#include <algorithm>

using Id = uint64_t;
using Buffer = std::vector<std::byte>;

enum class TypeId : Id {
    Uint,
    Float,
    String,
    Vector
};

// Helper для преобразования чисел в little-endian
template<typename T>
std::vector<std::byte> toLittleEndian(T value) {
    std::vector<std::byte> result(sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i) {
        result[i] = static_cast<std::byte>(value & 0xFF);
        value >>= 8;
    }
    return result;
}

// Helper для чтения чисел из little-endian
template<typename T>
T fromLittleEndian(const std::byte* data) {
    T result = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        result |= static_cast<T>(data[i]) << (i * 8);
    }
    return result;
}

// Базовый тип IntegerType
class IntegerType {
public:
    explicit IntegerType(uint64_t value) : value_(value) {}

    void serialize(Buffer& buffer) const {
        auto le = toLittleEndian(value_);
        buffer.insert(buffer.end(), le.begin(), le.end());
    }

    Buffer::const_iterator deserialize(Buffer::const_iterator begin, Buffer::const_iterator end) {
        if (std::distance(begin, end) < sizeof(uint64_t)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        value_ = fromLittleEndian<uint64_t>(&(*begin));
        return begin + sizeof(uint64_t);
    }

    uint64_t getValue() const { return value_; }

private:
    uint64_t value_;
};

// Базовый тип FloatType
class FloatType {
public:
    explicit FloatType(double value) : value_(value) {}

    void serialize(Buffer& buffer) const {
        auto le = toLittleEndian(*reinterpret_cast<uint64_t*>(&value_));
        buffer.insert(buffer.end(), le.begin(), le.end());
    }

    Buffer::const_iterator deserialize(Buffer::const_iterator begin, Buffer::const_iterator end) {
        if (std::distance(begin, end) < sizeof(double)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        uint64_t rawValue = fromLittleEndian<uint64_t>(&(*begin));
        value_ = *reinterpret_cast<double*>(&rawValue);
        return begin + sizeof(double);
    }

    double getValue() const { return value_; }

private:
    double value_;
};

// Базовый тип StringType
class StringType {
public:
    explicit StringType(const std::string& value) : value_(value) {}

    void serialize(Buffer& buffer) const {
        auto sizeLe = toLittleEndian(static_cast<uint64_t>(value_.size()));
        buffer.insert(buffer.end(), sizeLe.begin(), sizeLe.end());
        buffer.insert(buffer.end(), reinterpret_cast<const std::byte*>(value_.data()),
                      reinterpret_cast<const std::byte*>(value_.data() + value_.size()));
    }

    Buffer::const_iterator deserialize(Buffer::const_iterator begin, Buffer::const_iterator end) {
        if (std::distance(begin, end) < sizeof(uint64_t)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        uint64_t size = fromLittleEndian<uint64_t>(&(*begin));
        begin += sizeof(uint64_t);
        if (std::distance(begin, end) < static_cast<int64_t>(size)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        value_.assign(reinterpret_cast<const char*>(&(*begin)), size);
        return begin + size;
    }

    const std::string& getValue() const { return value_; }

private:
    std::string value_;
};

// Контейнерный тип VectorType
class VectorType {
public:
    template<typename Arg>
    void push_back(Arg&& val) {
        elements_.emplace_back(std::forward<Arg>(val));
    }

    void serialize(Buffer& buffer) const {
        auto sizeLe = toLittleEndian(static_cast<uint64_t>(elements_.size()));
        buffer.insert(buffer.end(), sizeLe.begin(), sizeLe.end());
        for (const auto& element : elements_) {
            element.serialize(buffer);
        }
    }

    Buffer::const_iterator deserialize(Buffer::const_iterator begin, Buffer::const_iterator end) {
        if (std::distance(begin, end) < sizeof(uint64_t)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        uint64_t size = fromLittleEndian<uint64_t>(&(*begin));
        begin += sizeof(uint64_t);
        elements_.clear();
        for (uint64_t i = 0; i < size; ++i) {
            Any any;
            begin = any.deserialize(begin, end);
            elements_.push_back(any);
        }
        return begin;
    }

    const std::vector<Any>& getElements() const { return elements_; }

private:
    std::vector<Any> elements_;
};

// Универсальный тип Any
class Any {
public:
    template<typename T>
    explicit Any(T&& value) : payload_(std::forward<T>(value)) {}

    void serialize(Buffer& buffer) const {
        std::visit([&buffer](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, IntegerType>) {
                buffer.insert(buffer.end(), toLittleEndian(static_cast<uint64_t>(TypeId::Uint)).begin(),
                              toLittleEndian(static_cast<uint64_t>(TypeId::Uint)).end());
            } else if constexpr (std::is_same_v<T, FloatType>) {
                buffer.insert(buffer.end(), toLittleEndian(static_cast<uint64_t>(TypeId::Float)).begin(),
                              toLittleEndian(static_cast<uint64_t>(TypeId::Float)).end());
            } else if constexpr (std::is_same_v<T, StringType>) {
                buffer.insert(buffer.end(), toLittleEndian(static_cast<uint64_t>(TypeId::String)).begin(),
                              toLittleEndian(static_cast<uint64_t>(TypeId::String)).end());
            } else if constexpr (std::is_same_v<T, VectorType>) {
                buffer.insert(buffer.end(), toLittleEndian(static_cast<uint64_t>(TypeId::Vector)).begin(),
                              toLittleEndian(static_cast<uint64_t>(TypeId::Vector)).end());
            }
            arg.serialize(buffer);
        }, payload_);
    }

    Buffer::const_iterator deserialize(Buffer::const_iterator begin, Buffer::const_iterator end) {
        if (std::distance(begin, end) < sizeof(uint64_t)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        TypeId typeId = static_cast<TypeId>(fromLittleEndian<uint64_t>(&(*begin)));
        begin += sizeof(uint64_t);
        switch (typeId) {
            case TypeId::Uint: {
                IntegerType value;
                begin = value.deserialize(begin, end);
                payload_ = value;
                break;
            }
            case TypeId::Float: {
                FloatType value;
                begin = value.deserialize(begin, end);
                payload_ = value;
                break;
            }
            case TypeId::String: {
                StringType value;
                begin = value.deserialize(begin, end);
                payload_ = value;
                break;
            }
            case TypeId::Vector: {
                VectorType value;
                begin = value.deserialize(begin, end);
                payload_ = value;
                break;
            }
            default:
                throw std::runtime_error("Unknown type ID");
        }
        return begin;
    }

    TypeId getPayloadTypeId() const {
        return std::visit([](auto&& arg) -> TypeId {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, IntegerType>) {
                return TypeId::Uint;
            } else if constexpr (std::is_same_v<T, FloatType>) {
                return TypeId::Float;
            } else if constexpr (std::is_same_v<T, StringType>) {
                return TypeId::String;
            } else if constexpr (std::is_same_v<T, VectorType>) {
                return TypeId::Vector;
            }
            throw std::runtime_error("Unknown type");
        }, payload_);
    }

    template<typename T>
    auto& getValue() const {
        return std::get<T>(payload_);
    }

    bool operator==(const Any& other) const {
        return payload_ == other.payload_;
    }

private:
    std::variant<IntegerType, FloatType, StringType, VectorType> payload_;
};

// Класс Serializator
class Serializator {
public:
    template<typename Arg>
    void push(Arg&& val) {
        storage_.emplace_back(std::forward<Arg>(val));
    }

    Buffer serialize() const {
        Buffer buffer;
        auto sizeLe = toLittleEndian(static_cast<uint64_t>(storage_.size()));
        buffer.insert(buffer.end(), sizeLe.begin(), sizeLe.end());
        for (const auto& element : storage_) {
            element.serialize(buffer);
        }
        return buffer;
    }

    static std::vector<Any> deserialize(const Buffer& buffer) {
        std::vector<Any> result;
        auto begin = buffer.cbegin();
        auto end = buffer.cend();
        if (std::distance(begin, end) < sizeof(uint64_t)) {
            throw std::runtime_error("Not enough data for deserialization");
        }
        uint64_t size = fromLittleEndian<uint64_t>(&(*begin));
        begin += sizeof(uint64_t);
        for (uint64_t i = 0; i < size; ++i) {
            Any any;
            begin = any.deserialize(begin, end);
            result.push_back(any);
        }
        return result;
    }

    const std::vector<Any>& getStorage() const {
        return storage_;
    }

private:
    std::vector<Any> storage_;
};

int main() {
    // Пример использования
    std::ifstream raw;
    raw.open("raw.bin", std::ios_base::in | std::ios_base::binary);
    if (!raw.is_open())
        return 1;
    raw.seekg(0, std::ios_base::end);
    std::streamsize size = raw.tellg();
    raw.seekg(0, std::ios_base::beg);

    Buffer buff(size);
    raw.read(reinterpret_cast<char*>(buff.data()), size);

    auto res = Serializator::deserialize(buff);

    Serializator s;
    for (auto&& i : res)
        s.push(i);

    std::cout << (buff == s.serialize()) << '\n';

    return 0;
}