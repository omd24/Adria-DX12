#include <algorithm>
#include "Camera.h"
#include "Core/Input.h"
#include "Math/Constants.h"
#include "Math/Halton.h"

using namespace DirectX;

namespace adria
{
	static const Vector3 DEFAULT_LOOK_VECTOR = Vector3::Forward;
	static const Vector3 DEFAULT_UP_VECTOR = Vector3::Up;
	static const Vector3 DEFAULT_RIGHT_VECTOR = Vector3::Right;

	Camera::Camera(CameraParameters const& desc) 
		: position(desc.position), aspect_ratio(1.0f), fov(desc.fov), near_plane(desc.far_plane), far_plane(desc.near_plane), enabled(true)
	{
		Vector3 look_vector = desc.look_at - position;
		look_vector.Normalize();
		orientation = Quaternion::LookRotation(look_vector, DEFAULT_UP_VECTOR);
		look_vector = Vector3::Transform(DEFAULT_LOOK_VECTOR, orientation);
	}

	Vector3 Camera::Forward() const
	{
		return Vector3::Transform(DEFAULT_LOOK_VECTOR, orientation);
	}

	Vector2 Camera::Jitter(uint32 frame_index) const
	{
		Vector2 jitter{};
		constexpr HaltonSequence<16, 2> x;
		constexpr HaltonSequence<16, 3> y;
		jitter.x = x[frame_index % 16] - 0.5f;
		jitter.y = y[frame_index % 16] - 0.5f;
		return jitter;
	}
	float Camera::Near() const
	{
		return near_plane;
	}
	float Camera::Far() const
	{
		return far_plane;
	}
	float Camera::Fov() const
	{
		return fov;
	}
	float Camera::AspectRatio() const
	{
		return aspect_ratio;
	}

	void Camera::Update(float dt)
	{
		if (!enabled) return;
		if (g_Input.GetKey(KeyCode::Space)) return;

		if (g_Input.GetKey(KeyCode::MouseRight))
		{
			float dx = g_Input.GetMouseDeltaX();
			float dy = g_Input.GetMouseDeltaY();
			Quaternion yaw_quaternion = Quaternion::CreateFromYawPitchRoll(0, dy * dt * 0.25f, 0);
			Quaternion pitch_quaternion = Quaternion::CreateFromYawPitchRoll(dx * dt * 0.25f, 0, 0);
			orientation = yaw_quaternion * orientation * pitch_quaternion;
		}

		Vector3 movement{};
		if (g_Input.GetKey(KeyCode::W)) movement.z += 1.0f;
		if (g_Input.GetKey(KeyCode::S)) movement.z -= 1.0f;
		if (g_Input.GetKey(KeyCode::D)) movement.x += 1.0f;
		if (g_Input.GetKey(KeyCode::A)) movement.x -= 1.0f;
		if (g_Input.GetKey(KeyCode::Q)) movement.y += 1.0f;
		if (g_Input.GetKey(KeyCode::E)) movement.y -= 1.0f;
		movement = Vector3::Transform(movement, orientation);
		velocity = Vector3::SmoothStep(velocity, movement, 0.35f);

		float speed_factor = 1.0f;
		if (g_Input.GetKey(KeyCode::ShiftLeft)) speed_factor *= 5.0f;
		if (g_Input.GetKey(KeyCode::CtrlLeft))  speed_factor *= 0.2f;
		position += velocity * dt * speed_factor * 25.0f;

		Matrix view_inverse = Matrix::CreateFromQuaternion(orientation) * Matrix::CreateTranslation(position);
		view_inverse.Invert(view_matrix);
		SetProjectionMatrix(fov, aspect_ratio, near_plane, far_plane);
	}
	void Camera::Zoom(int32 increment)
	{
		if (!enabled) return;
		fov -= XMConvertToRadians(increment * 1.0f);
		fov = std::clamp(fov, 0.00005f, pi_div_2<float>);
		SetProjectionMatrix(fov, aspect_ratio, near_plane, far_plane);
	}
	void Camera::OnResize(uint32 w, uint32 h)
	{
		SetAspectRatio(static_cast<float>(w) / h);
	}

	void Camera::SetAspectRatio(float ar)
	{
		aspect_ratio = ar;
		SetProjectionMatrix(fov, aspect_ratio, near_plane, far_plane);
	}
	void Camera::SetFov(float _fov)
	{
		fov = _fov;
		SetProjectionMatrix(fov, aspect_ratio, near_plane, far_plane);
	}
	void Camera::SetNearAndFar(float n, float f)
	{
		near_plane = n;
		far_plane = f;
		SetProjectionMatrix(fov, aspect_ratio, near_plane, far_plane);
	}
	void Camera::SetPosition(Vector3 const& pos)
	{
		position = pos;
	}

	Matrix Camera::View() const
	{
		return view_matrix;
	}
	Matrix Camera::Proj() const
	{
		return projection_matrix;
	}
	Matrix Camera::ViewProj() const
	{
		return view_matrix * projection_matrix;
	}
	BoundingFrustum Camera::Frustum() const
	{
		BoundingFrustum frustum(Proj());
		if (frustum.Far < frustum.Near) std::swap(frustum.Far, frustum.Near);
		frustum.Transform(frustum, view_matrix.Invert());
		return frustum;
	}
	void Camera::SetProjectionMatrix(float fov, float aspect, float zn, float zf)
	{
		projection_matrix = XMMatrixPerspectiveFovLH(fov, aspect, zn, zf);
	}
}