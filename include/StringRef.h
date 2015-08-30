#pragma once

// Wrapper around a {pointer, length} pair to a string on the heap.

namespace slang {

class StringRef {
public:
    StringRef() 
        : ptr(nullptr), len(0) {
    }

    StringRef(const char* ptr, uint32_t length)
        : ptr(ptr), len(length) {
    }

    const char* begin() const { return ptr; }
    const char* end() const { return ptr + len; }

    uint32_t length() const { return len; }
    bool empty() const { return len == 0; }

    StringRef subString(uint32_t startIndex, uint32_t length) const {
        ASSERT(startIndex + length <= len);
        return StringRef(ptr + startIndex, length);
    }

    char operator[](uint32_t index) const {
        ASSERT(index < len);
        return ptr[index];
    }

    friend bool operator==(const StringRef& lhs, const std::string& rhs) {
        if (lhs.len != rhs.length())
            return false;

        return rhs.compare(0, rhs.length(), lhs.ptr, lhs.len) == 0;
    }

    friend bool operator==(const StringRef& lhs, const StringRef& rhs) {
        if (lhs.len != rhs.len)
            return false;

        return strncmp(lhs.ptr, rhs.ptr, std::min(lhs.len, rhs.len)) == 0;
    }

    friend bool operator==(const StringRef& lhs, const char* rhs) {
        const char* ptr = lhs.ptr;
        for (uint32_t i = 0; i < lhs.len; i++) {
            if (*ptr++ != *rhs++)
                return false;
        }

        // rhs should be null now, otherwise the lengths differ
        return *rhs == 0;
    }

    friend bool operator==(const std::string& lhs, const StringRef& rhs) { return operator==(rhs, lhs); }
    friend bool operator==(const char* lhs, const StringRef& rhs) { return operator==(rhs, lhs); }

    friend bool operator!=(const StringRef& lhs, const std::string& rhs) { return !operator==(lhs, rhs); }
    friend bool operator!=(const std::string& lhs, const StringRef& rhs) { return !operator==(lhs, rhs); }
    friend bool operator!=(const StringRef& lhs, const char* rhs) { return !operator==(lhs, rhs); }
    friend bool operator!=(const char* lhs, const StringRef& rhs) { return !operator==(lhs, rhs); }
    friend bool operator!=(const StringRef& lhs, const StringRef& rhs) { return !operator==(lhs, rhs); }

private:
    const char* ptr;
    uint32_t len;
};

}