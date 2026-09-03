// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace CnaCity
{
    /**
     * @brief One traversal that both writes and reads, so the two cannot drift apart.
     *
     * Every save format that has a `Write` function and a matching `Read` function eventually has
     * one field in one of them and not the other, and the symptom is a file that loads into a
     * plausible-looking world with everything after that field shifted by four bytes. There is one
     * function per type here and it is used for both directions: a field that is added is added
     * once, and a field that is forgotten is forgotten in both directions, which is a format
     * mismatch the version check catches rather than silent corruption.
     *
     * Little-endian and native layout: this is a scratch file for a benchmark on one machine, not
     * an interchange format. The header records the sizes it was written with so that a file from
     * a different build is refused rather than misread.
     */
    class Archive
    {
    public:
        /** @brief An archive that appends to @p buffer. */
        explicit Archive(std::vector<std::uint8_t>& buffer) : write_(&buffer) {}

        /** @brief An archive that consumes @p data. */
        Archive(const std::uint8_t* data, std::size_t size) : read_(data), size_(size) {}

        [[nodiscard]] bool writing() const { return write_ != nullptr; }
        [[nodiscard]] bool ok() const { return ok_; }
        [[nodiscard]] std::size_t consumed() const { return cursor_; }

        /** @brief A trivially copyable value: an int, a float, an enum, a small struct. */
        template <class T>
        void Pod(T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "Archive::Pod needs a flat type");
            if (!ok_) return;
            if (write_ != nullptr)
            {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
                write_->insert(write_->end(), bytes, bytes + sizeof(T));
            }
            else
            {
                if (cursor_ + sizeof(T) > size_) { ok_ = false; return; }
                std::memcpy(&value, read_ + cursor_, sizeof(T));
                cursor_ += sizeof(T);
            }
        }

        /** @brief A vector of flat values, length first. */
        template <class T>
        void Vector(std::vector<T>& values)
        {
            static_assert(std::is_trivially_copyable_v<T>, "Archive::Vector needs a flat type");
            if (!ok_) return;
            auto count = static_cast<std::uint64_t>(values.size());
            Pod(count);
            if (!ok_) return;
            if (write_ != nullptr)
            {
                if (count == 0) return;
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(values.data());
                write_->insert(write_->end(), bytes, bytes + count * sizeof(T));
            }
            else
            {
                // A length is the one field an attacker or a truncated file controls, so it is
                // checked against what is actually left rather than trusted into a resize.
                if (count > (size_ - cursor_) / sizeof(T)) { ok_ = false; return; }
                values.resize(static_cast<std::size_t>(count));
                if (count == 0) return;
                std::memcpy(values.data(), read_ + cursor_, static_cast<std::size_t>(count) * sizeof(T));
                cursor_ += static_cast<std::size_t>(count) * sizeof(T);
            }
        }

        /** @brief A vector of vectors of flat values -- the per-station and per-stop queues. */
        template <class T>
        void NestedVector(std::vector<std::vector<T>>& values)
        {
            if (!ok_) return;
            auto count = static_cast<std::uint64_t>(values.size());
            Pod(count);
            if (!ok_) return;
            if (!writing())
            {
                if (count > size_ - cursor_) { ok_ = false; return; }
                values.assign(static_cast<std::size_t>(count), {});
            }
            for (std::uint64_t i = 0; i < count && ok_; ++i)
                Vector(values[static_cast<std::size_t>(i)]);
        }

        void Text(std::string& value)
        {
            if (!ok_) return;
            auto count = static_cast<std::uint64_t>(value.size());
            Pod(count);
            if (!ok_) return;
            if (writing())
            {
                if (count == 0) return;
                write_->insert(write_->end(), value.begin(), value.end());
            }
            else
            {
                if (count > size_ - cursor_) { ok_ = false; return; }
                value.assign(reinterpret_cast<const char*>(read_ + cursor_),
                             static_cast<std::size_t>(count));
                cursor_ += static_cast<std::size_t>(count);
            }
        }

        /**
         * @brief A marker that must be there when reading.
         *
         * One of these between sections costs four bytes and turns "the world loaded strangely"
         * into "the file is corrupt after the agents".
         */
        void Fence(std::uint32_t tag)
        {
            std::uint32_t value = tag;
            Pod(value);
            if (ok_ && !writing() && value != tag) ok_ = false;
        }

    private:
        std::vector<std::uint8_t>* write_ = nullptr;
        const std::uint8_t* read_ = nullptr;
        std::size_t size_ = 0;
        std::size_t cursor_ = 0;
        bool ok_ = true;
    };
}
