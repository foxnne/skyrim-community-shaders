#pragma once

#include "../DX12SwapChain.h"

#include <array>
#include <cstdint>
#include <d3d11_4.h>
#include <d3d12.h>

/** @brief D3D12 fence shared into D3D11; value is the last value handed out by Next(). */
struct SharedFence
{
	static constexpr DWORD kRemovalPollMs = 100;
	/** @brief CPU bound for one fence value; a wedged GPU must not hang the render thread. */
	static constexpr DWORD kFenceTimeoutMs = 5000;
	winrt::com_ptr<ID3D12Fence> fence12;
	winrt::com_ptr<ID3D11Fence> fence11;
	uint64_t value = 0;

	/** @brief Returns the next value to signal, advancing the monotonic counter. */
	uint64_t Next() { return ++value; }
	/** @brief Releases both fence interfaces and returns the counter to zero. */
	void Reset()
	{
		fence12 = nullptr;
		fence11 = nullptr;
		value = 0;
	}
	/** @brief Outcome of a bounded CPU wait, so a caller can tell a slow GPU from a broken wait. */
	enum class WaitOutcome : uint8_t
	{
		kComplete,  ///< The fence reached the value.
		kTimeout,   ///< The fence did not reach the value within the bound.
		kFailed     ///< The wait itself failed: no event, no completion registration, or WAIT_FAILED.
	};

	/** @brief Creates and names the fence; throws on failure without leaking the NT handle. */
	void Create(ID3D12Device* a_device12, ID3D11Device5* a_device11, const char* a_name);
	/** @brief Waits on the CPU up to a_timeoutMs, polling device removal via fence12's own device.
	 *  Trivially kComplete when the fence is unset or a_value is 0 (nothing to wait for).
	 *  @param a_error Receives the error behind a kFailed outcome: the Win32 error from the wait,
	 *         or the HRESULT a failed completion registration returned; may be null. */
	WaitOutcome CpuWaitOutcome(uint64_t a_value, DWORD a_timeoutMs, DWORD* a_error = nullptr) const;
	/** @brief True only when the fence reached a_value inside the bound. */
	bool CpuWait(uint64_t a_value, DWORD a_timeoutMs) const { return CpuWaitOutcome(a_value, a_timeoutMs) == WaitOutcome::kComplete; }
};

namespace NR
{
	/** @brief Owns the same-adapter NR queue and D3D11/D3D12 synchronization. */
	class D3D12Interop
	{
	public:
		/** @brief Command-allocator ring depth; a slot is reused once its submission retires. */
		static constexpr uint32_t kFramesInFlight = 3;

		/** @brief Creates the device, queue, ring and shared fence on the renderer's adapter. */
		void Initialize();
		/** @brief Creates a named, view-bearing D3D11 texture shared with the NR device. */
		std::unique_ptr<WrappedResource> CreateTexture(uint32_t width, uint32_t height, DXGI_FORMAT format, const std::string& name);
		/** @brief Acquires a retired allocator and queues the D3D11 input dependency. */
		ID3D12GraphicsCommandList* Begin();
		/** @brief Submits NR commands and queues the D3D11 output dependency. */
		void End();
		/**
		 * @brief Returns a command list that runs on a second compute queue concurrently with Begin()'s list,
		 *        submitted by the same End(). Only a compute queue overlaps NR's launch-bound kernels; a second
		 *        direct queue serializes with the first. Call between Begin() and End().
		 * @return Null when the queue cannot be created; the caller then records on Begin()'s list.
		 */
		ID3D12GraphicsCommandList* ParallelList();
		/** @brief Whether the last End() submitted a list on the second queue. */
		[[nodiscard]] bool UsedParallelLastSubmit() const { return parallelSubmitted; }
		/** @brief Stops using the second queue for the rest of the session, after a failure that may have come from it. */
		void DisableParallel() { parallelFailed = true; }
		/**
		 * @brief Retires all submitted work on both APIs.
		 *        Throws rather than releasing anything: on a failure the caller must keep the
		 *        resources alive, because the GPU may still reference them.
		 */
		void Drain();

		/** @brief Returns the device used by NGX. */
		ID3D12Device* Device() const { return device.get(); }
		/** @brief The D3D11.1 device the interop already holds; a derived type satisfies base-interface needs. */
		ID3D11Device1* Device1() const { return device11.get(); }
		/** @brief The D3D11.1 context the interop already holds; a derived type satisfies base-interface needs. */
		ID3D11DeviceContext1* Context1() const { return context.get(); }
		/** @brief LUID of the adapter that device was created on, for the caller's success log. */
		[[nodiscard]] LUID AdapterLuid() const { return adapterLuid; }
		/** @brief Asks the device whether it was removed, so a caller can release instead of waiting. */
		[[nodiscard]] bool DeviceRemoved() const { return device && FAILED(device->GetDeviceRemovedReason()); }
		/** @brief Returns the most recently issued shared-fence value. */
		uint64_t SubmittedFence() const { return fence.value; }
		/** @brief Samples GPU progress without waiting. */
		uint64_t CompletedFence() const { return fence.fence12 ? fence.fence12->GetCompletedValue() : 0; }

	private:
		struct Commands
		{
			winrt::com_ptr<ID3D12CommandAllocator> allocator;
			winrt::com_ptr<ID3D12GraphicsCommandList> list;
			uint64_t completion = 0;
		};

		/** @brief Creates a named allocator and closed command list of one type for every slot of a ring. */
		void CreateRing(std::array<Commands, kFramesInFlight>& ring, D3D12_COMMAND_LIST_TYPE type, const wchar_t* allocatorName, const wchar_t* listName);
		/** @brief Blocks the CPU until completion retires; throws on a timeout or a removed device. */
		void Wait(SharedFence& target, uint64_t completion);
		/** @brief Releases every interop resource; drain first or the GPU may still read them. */
		void Reset();

		winrt::com_ptr<ID3D11Device5> device11;
		winrt::com_ptr<ID3D11DeviceContext4> context;
		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12CommandQueue> parallelQueue;
		std::array<Commands, kFramesInFlight> parallelCommands;
		/** @brief Completion timeline signaled only by the parallel queue. */
		SharedFence parallelFence;
		uint64_t inputReady = 0;
		bool parallelUsed = false;
		bool parallelSubmitted = false;
		bool parallelFailed = false;
		SharedFence fence;
		std::array<Commands, kFramesInFlight> commands;
		LUID adapterLuid{};
		uint32_t cursor = 0;
	};
}
