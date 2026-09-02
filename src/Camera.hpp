// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstdint>

#include "CityMath.hpp"
#include "Microsoft/Xna/Framework/MathHelper.hpp"
#include "Microsoft/Xna/Framework/Matrix.hpp"
#include "Microsoft/Xna/Framework/Vector3.hpp"

namespace CnaCity
{
    /** @brief How the camera is being driven. The key that selects each is in the README. */
    enum class CameraMode : std::uint8_t
    {
        Free = 0,    ///< WASD and the mouse; goes anywhere.
        Orbit,       ///< Circles the downtown skyline.
        Follow,      ///< Rides on the shoulder of one citizen for their whole day.
        Street,      ///< Standing on a pavement, watching the city go past.
        Cinematic    ///< A slow scripted sweep, for capture.
    };

    inline constexpr int kCameraModeCount = 5;

    [[nodiscard]] inline const char* CameraModeName(CameraMode mode)
    {
        switch (mode)
        {
            case CameraMode::Free:      return "free";
            case CameraMode::Orbit:     return "orbit";
            case CameraMode::Follow:    return "follow";
            case CameraMode::Street:    return "street";
            case CameraMode::Cinematic: return "cinematic";
        }
        return "?";
    }

    /**
     * @brief A yaw/pitch camera with a target, plus the projection the city needs.
     *
     * The far plane is the interesting number here. A city three kilometres across, seen from a
     * camera that may be at street level or four hundred metres up, wants six kilometres of range
     * -- and with a one-metre near plane that is a depth ratio of six thousand, which is exactly
     * where a 24-bit depth buffer starts losing pavements into roads. The near plane is therefore
     * pushed out to 0.6 m and the far plane is kept as tight as the mode allows.
     */
    struct Camera
    {
        Microsoft::Xna::Framework::Vector3 position{0.0f, 180.0f, 420.0f};
        float yaw = -1.5708f;      ///< Radians; 0 looks along +X.
        float pitch = -0.42f;      ///< Radians; negative looks down.
        float fovY = 1.0472f;      ///< 60 degrees.
        float nearPlane = 0.6f;
        float farPlane = 5200.0f;
        float aspect = 16.0f / 9.0f;
        /// Rolls slightly into a fast turn. Only the cinematic and follow modes use it, and it is
        /// the cheapest thing in the file that makes a camera look like it was operated.
        float roll = 0.0f;

        [[nodiscard]] Microsoft::Xna::Framework::Vector3 Forward() const
        {
            const float cosPitch = std::cos(pitch);
            return Microsoft::Xna::Framework::Vector3(std::cos(yaw) * cosPitch, std::sin(pitch),
                                                      std::sin(yaw) * cosPitch);
        }

        [[nodiscard]] Microsoft::Xna::Framework::Vector3 Right() const
        {
            return Microsoft::Xna::Framework::Vector3(std::cos(yaw + 1.5708f), 0.0f,
                                                      std::sin(yaw + 1.5708f));
        }

        [[nodiscard]] Microsoft::Xna::Framework::Matrix View() const
        {
            const Microsoft::Xna::Framework::Vector3 forward = Forward();
            const Microsoft::Xna::Framework::Vector3 target(position.X + forward.X,
                                                            position.Y + forward.Y,
                                                            position.Z + forward.Z);
            const Microsoft::Xna::Framework::Vector3 right = Right();
            const Microsoft::Xna::Framework::Vector3 up(
                -std::sin(roll) * right.X, std::cos(roll), -std::sin(roll) * right.Z);
            return Microsoft::Xna::Framework::Matrix::CreateLookAt(position, target, up);
        }

        [[nodiscard]] Microsoft::Xna::Framework::Matrix Projection() const
        {
            return Microsoft::Xna::Framework::Matrix::CreatePerspectiveFieldOfView(
                fovY, aspect, nearPlane, farPlane);
        }

        void LookAt(const Microsoft::Xna::Framework::Vector3& target)
        {
            const Microsoft::Xna::Framework::Vector3 delta(target.X - position.X,
                                                           target.Y - position.Y,
                                                           target.Z - position.Z);
            const float horizontal = std::sqrt(delta.X * delta.X + delta.Z * delta.Z);
            yaw = std::atan2(delta.Z, delta.X);
            pitch = std::atan2(delta.Y, std::max(0.001f, horizontal));
        }

        /** @brief Moves toward @p target with a frame-rate-independent exponential ease. */
        void EaseTo(const Microsoft::Xna::Framework::Vector3& target, float dt, float halfLife)
        {
            const float t = 1.0f - std::exp(-dt * 0.6931472f / std::max(0.001f, halfLife));
            position = Microsoft::Xna::Framework::Vector3(position.X + (target.X - position.X) * t,
                                                          position.Y + (target.Y - position.Y) * t,
                                                          position.Z + (target.Z - position.Z) * t);
        }
    };
}
