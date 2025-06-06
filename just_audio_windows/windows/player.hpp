#pragma comment(lib, "windowsapp")

#include <chrono>

// This must be included before many other Windows headers.
#include <windows.h>

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <ppltasks.h>
#include <string>



#define TO_MILLISECONDS(timespan) timespan.count() / 10000
#define TO_MICROSECONDS(timespan) TO_MILLISECONDS(timespan) * 1000

using flutter::EncodableMap;
using flutter::EncodableValue;

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media;
using namespace concurrency;


using winrt::Windows::Media::Core::MediaSource;

// Looks for |key| in |map|, returning the associated value if it is present, or
// a nullptr if not.
//
// The variant types are mapped with Dart types in following ways:
// std::monostate       -> null
// bool                 -> bool
// int32_t              -> int
// int64_t              -> int
// double               -> double
// std::string          -> String
// std::vector<uint8_t> -> Uint8List
// std::vector<int32_t> -> Int32List
// std::vector<int64_t> -> Int64List
// std::vector<float>   -> Float32List
// std::vector<double>  -> Float64List
// EncodableList        -> List
// EncodableMap         -> Map
const EncodableValue* ValueOrNull(const EncodableMap& map, const char* key)
{
	auto it = map.find(EncodableValue(key));
	if (it == map.end())
	{
		return nullptr;
	}
	return &(it->second);
}

// Converts a std::string to std::wstring
auto TO_WIDESTRING = [](std::string string) -> std::wstring
	{
		if (string.empty())
		{
			return std::wstring();
		}
		int32_t target_length =
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
				static_cast<int32_t>(string.length()), nullptr, 0);
		if (target_length == 0)
		{
			return std::wstring();
		}
		std::wstring utf16_string;
		utf16_string.resize(target_length);
		int32_t converted_length =
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(),
				static_cast<int32_t>(string.length()),
				utf16_string.data(), target_length);
		if (converted_length == 0)
		{
			return std::wstring();
		}
		return utf16_string;
	};

class AudioEventSink
{
public:
	// Prevent copying.
	AudioEventSink(AudioEventSink const&) = delete;
	AudioEventSink& operator=(AudioEventSink const&) = delete;
	AudioEventSink(flutter::BinaryMessenger* messenger, const std::string& id)
	{
		auto event_channel =
			std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(messenger, id, &flutter::StandardMethodCodec::GetInstance());

		auto event_handler = std::make_unique<flutter::StreamHandlerFunctions<>>(
			[self = this](const EncodableValue* arguments, std::unique_ptr<flutter::EventSink<>>&& events) -> std::unique_ptr<flutter::StreamHandlerError<>>
			{
				self->sink = std::move(events);
				return nullptr;
			},
			[self = this](const EncodableValue* arguments) -> std::unique_ptr<flutter::StreamHandlerError<>>
			{
				self->sink.reset();
				return nullptr;
			});

		event_channel->SetStreamHandler(std::move(event_handler));
	}

	void Success(const EncodableValue& event)
	{
		if (sink)
		{
			sink->Success(event);
		}
	}

	void Error(const std::string& error_code,
		const std::string& error_message)
	{
		if (sink)
		{
			sink->Error(error_code, error_message);
		}
	}

private:
	std::unique_ptr<flutter::EventSink<>> sink = nullptr;
};

class AudioPlayer
{
private:
	/* data */
public:
	std::string id;
	Playback::MediaPlayer mediaPlayer{};
	Playback::MediaPlaybackList mediaPlaybackList{};
	bool closed = false;
	winrt::Windows::Devices::Enumeration::DeviceInformation currentDevice = nullptr;
	std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> player_channel_;
	std::unique_ptr<AudioEventSink> event_sink_ = nullptr;
	std::unique_ptr<AudioEventSink> data_sink_ = nullptr;

	AudioPlayer(std::string idx, flutter::BinaryMessenger* messenger)
	{
		id = idx;

		// Set up channels
		player_channel_ =
			std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
				messenger, "com.ryanheise.just_audio.methods." + idx,
				&flutter::StandardMethodCodec::GetInstance());

		player_channel_->SetMethodCallHandler(
			[player = this](const auto& call, auto result)
			{
				player->HandleMethodCall(call, std::move(result));
			});

		event_sink_ = std::make_unique<AudioEventSink>(messenger, "com.ryanheise.just_audio.events." + idx);
		data_sink_ = std::make_unique<AudioEventSink>(messenger, "com.ryanheise.just_audio.data." + idx);

		mediaPlayer.CommandManager().IsEnabled(false);

		/// Set up event callbacks
		// Playback event
		mediaPlayer.PlaybackSession().PlaybackStateChanged([=](auto, const auto& args) -> void
			{ broadcastState(); });

		// Player error event
		mediaPlayer.MediaFailed([=](auto, const Playback::MediaPlayerFailedEventArgs& args) -> void
			{
				std::string errorMessage = winrt::to_string(args.ErrorMessage());

				std::cerr << "[just_audio_windows] Media error: " << errorMessage << std::endl;

				auto code = "unknown";

				switch (args.Error()) {
				case Playback::MediaPlayerError::Unknown:
					break;
				case Playback::MediaPlayerError::Aborted:
					code = "aborted";
					break;
				case Playback::MediaPlayerError::NetworkError:
					code = "networkError";
					break;
				case Playback::MediaPlayerError::DecodingError:
					code = "decodingError";
					break;
				case Playback::MediaPlayerError::SourceNotSupported:
					code = "sourceNotSupported";
					break;
				}

				event_sink_->Error(code, errorMessage); });

		mediaPlaybackList.MaxPlayedItemsToKeepOpen(2);
		mediaPlaybackList.CurrentItemChanged([=](auto, const auto& args) -> void
			{ broadcastState(); });
		mediaPlaybackList.ItemFailed([=](auto, const Playback::MediaPlaybackItemFailedEventArgs& args) -> void
			{
				auto error = winrt::hresult_error(args.Error().ExtendedError());

				auto message = winrt::to_string(error.message());

				std::cerr << "[just_audio_windows] Item error: " << message << std::endl;

				auto code = "unknown";

				switch (args.Error().ErrorCode()) {
				case Playback::MediaPlaybackItemErrorCode::Aborted:
					code = "aborted";
					break;
				case Playback::MediaPlaybackItemErrorCode::NetworkError:
					code = "networkError";
					break;
				case Playback::MediaPlaybackItemErrorCode::DecodeError:
					code = "decodeError";
					break;
				case Playback::MediaPlaybackItemErrorCode::SourceNotSupportedError:
					code = "sourceNotSupportedError";
					break;
				case Playback::MediaPlaybackItemErrorCode::EncryptionError:
					code = "encryptionError";
					break;
				}

				event_sink_->Error(code, message); });
	}
	~AudioPlayer()
	{
		mediaPlayer.Close();
		closed = true;
	}

	bool HasPlayerId(std::string playerId)
	{
		return id == playerId;
	}

	void HandleMethodCall(
		const flutter::MethodCall<flutter::EncodableValue>& method_call,
		std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
	{
		const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());

		std::cerr << "[just_audio_windows] Called " << method_call.method_name() << std::endl;

		if (method_call.method_name().compare("load") == 0)
		{
			const auto* audioSourceData = std::get_if<flutter::EncodableMap>(ValueOrNull(*args, "audioSource"));
			const auto* initialPosition = std::get_if<int>(ValueOrNull(*args, "initialPosition"));
			const auto* initialIndex = std::get_if<int>(ValueOrNull(*args, "initialIndex"));

			try
			{
				loadSource(*audioSourceData);
			}
			catch (char* error)
			{
				return result->Error("load_error", error);
			}

			if (initialIndex != nullptr)
			{
				seekToItem((uint32_t)*initialIndex);
			}

			if (initialPosition != nullptr)
			{
				seekToPosition(*initialPosition);
			}

			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("play") == 0)
		{
			mediaPlayer.Play();
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("pause") == 0)
		{
			mediaPlayer.Pause();
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setVolume") == 0)
		{
			const auto* volume = std::get_if<double>(ValueOrNull(*args, "volume"));
			if (!volume)
			{
				return result->Error("volume_error", "volume argument missing");
			}
			if (closed) {
				std::cout << "Player is closed, aborting volume set" << std::endl;
				return result->Success(flutter::EncodableMap{{"error", "volume - player is closed"}});
			}
			float volumeFloat = (float)*volume;
			try {
				// Extra safeguard: if AudioDevice is nullptr, don't set volume
				auto device = mediaPlayer.AudioDevice();
				
				std::cout<< "device get" << std::endl;
				if (!device) {
					std::cerr << "[just_audio_windows] setVolume error: no device" << std::endl;
					result->Success(flutter::EncodableMap{{"error", "volume - no device"}});
					return;
				}
				std::cout << "Setting volume to " << volumeFloat << std::endl;
				mediaPlayer.Volume(volumeFloat);
			}
			catch (...) {
				std::cerr << "[just_audio_windows] setVolume error" << std::endl;
				result->Success(flutter::EncodableMap{{"error", "volume - something went wrong"}});
			}
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setSpeed") == 0)
		{
			const auto* speed = std::get_if<double>(ValueOrNull(*args, "speed"));
			if (!speed)
			{
				return result->Error("speed_error", "speed argument missing");
			}
			float speedFloat = (float)*speed;
			mediaPlayer.PlaybackRate(speedFloat);
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setPitch") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setSkipSilence") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setLoopMode") == 0)
		{
			const auto* loopModePtr = std::get_if<int32_t>(ValueOrNull(*args, "loopMode"));
			if (loopModePtr == nullptr)
			{
				return result->Error("loopMode_error", "loopMode argument missing");
			}

			switch (*loopModePtr)
			{
			case 0: // off
				mediaPlayer.IsLoopingEnabled(false);
				mediaPlaybackList.AutoRepeatEnabled(false);
				break;
			case 1: // one
				mediaPlayer.IsLoopingEnabled(true);
				mediaPlaybackList.AutoRepeatEnabled(false);
				break;
			case 2: // all
				mediaPlayer.IsLoopingEnabled(false);
				mediaPlaybackList.AutoRepeatEnabled(true);
				break;
			default:
				return result->Error("loopMode_error", "loopMode is invalid");
			}

			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setShuffleMode") == 0)
		{
			const auto* shuffleModePtr = std::get_if<int32_t>(ValueOrNull(*args, "shuffleMode"));
			if (shuffleModePtr == nullptr)
			{
				return result->Error("shuffleMode_error", "shuffleMode argument missing");
			}

			switch (*shuffleModePtr)
			{
			case 0: // none
				mediaPlaybackList.ShuffleEnabled(false);
				break;
			case 1: // all
				mediaPlaybackList.ShuffleEnabled(true);
				break;
			default:
				return result->Error("shuffleMode_error", "shuffleMode is invalid");
			}

			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setShuffleOrder") == 0)
		{
			const auto* source = std::get_if<flutter::EncodableMap>(ValueOrNull(*args, "audioSource"));

			setShuffleOrder(*source);

			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setAutomaticallyWaitsToMinimizeStalling") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setCanUseNetworkResourcesForLiveStreamingWhilePaused") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setPreferredPeakBitRate") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("seek") == 0)
		{
			const auto* index = std::get_if<int>(ValueOrNull(*args, "index"));
			if (index != nullptr)
			{
				seekToItem((uint32_t)*index);
			}

			const auto* position = std::get_if<int>(ValueOrNull(*args, "position"));
			if (position != nullptr)
			{
				seekToPosition(*position);
			}
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("concatenatingInsertAll") == 0)
		{
			const auto* index = std::get_if<int>(ValueOrNull(*args, "index"));
			const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(*args, "children"));

			auto items = mediaPlaybackList.Items();

			int currentIndex = *index;
			for (auto& child : *children)
			{
				const auto* childMap = std::get_if<flutter::EncodableMap>(&child);
				auto mediaSource = createMediaPlaybackItem(*childMap);
				auto item = Playback::MediaPlaybackItem(mediaSource);

				items.InsertAt(currentIndex, item);
				currentIndex++;
			}

			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("concatenatingRemoveRange") == 0)
		{
			const auto* start = std::get_if<int>(ValueOrNull(*args, "startIndex"));
			const auto* end = std::get_if<int>(ValueOrNull(*args, "endIndex")); // Does not include this item

			int startIndex = *start;
			int endIndex = *end;

			auto items = mediaPlaybackList.Items();
			auto size = (int)items.Size();

			if (endIndex > startIndex && startIndex >= 0 && endIndex <= size)
			{
				int count = endIndex - startIndex;

				for (int i = 0; i < count; i++)
				{
					// The item to remove should always be located at `startIndex`.
					items.RemoveAt(startIndex);
				}
				return result->Success(flutter::EncodableMap());
			}
			else
			{
				return result->Error("concatenatingRemoveRange_error", "invalid range");
			}
		}
		else if (method_call.method_name().compare("concatenatingMove") == 0)
		{
			const auto* from = std::get_if<int>(ValueOrNull(*args, "currentIndex"));
			const auto* to = std::get_if<int>(ValueOrNull(*args, "newIndex"));

			auto items = mediaPlaybackList.Items();
			auto size = (int)items.Size();

			int currentIndex = *from;
			int newIndex = *to;

			auto item = items.GetAt(currentIndex);

			if (currentIndex >= size || newIndex > size)
			{
				return result->Error("concatenatingMove_error", "index out of bounds");
			}

			items.RemoveAt(currentIndex);
			items.InsertAt(newIndex, item);
			// Do nothing if the two equals
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setAndroidAudioAttributes") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("audioEffectSetEnabled") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("androidLoudnessEnhancerSetTargetGain") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("androidEqualizerGetParameters") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("androidEqualizerBandSetGain") == 0)
		{
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("dispose") == 0)
		{
			mediaPlayer.Close();
			closed = true;
			result->Success(flutter::EncodableMap());
		}
		else if (method_call.method_name().compare("setOutputDevice") == 0)
		{
			const std::string* deviceID = std::get_if<std::string>(ValueOrNull(*args, "deviceID"));

			if (deviceID)
			{

				std::cout << "DEVICE ID FETCHED: " << *deviceID << std::endl;
				std::string deviceIdValue = *deviceID; // Copy the value


				std::thread([this, deviceIdValue]()
					{
						try {
							std::cout << "Fetching devices in background thread..." << std::endl;

							// Retrieve all audio output devices (long-running operation)
							auto devices = DeviceInformation::FindAllAsync().get();
							std::cout << "Devices fetched." << std::endl;

							// Find the device with the specified ID
							winrt::Windows::Devices::Enumeration::DeviceInformation selectedDevice = nullptr;
							for (auto device : devices) {
								std::string deviceIdStr = winrt::to_string(device.Id());
								if (deviceIdStr.find(deviceIdValue) != std::wstring::npos) {
									std::cout << "Found selected device." << std::endl;
									selectedDevice = device;
									break;
								}
							}

							if (!selectedDevice) {
								std::cout << "No device found with the specified ID, exiting background thread." << std::endl;
								return;
							}

							if (closed) {
								std::cerr << "Player is already closed, aborting device set" << std::endl;
								return;
							}

							winrt::Windows::System::DispatcherQueueController controller = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
							winrt::Windows::System::DispatcherQueue dispatcher = controller.DispatcherQueue();

							if (!dispatcher) {
								std::cerr << "DispatcherQueue is not available on the current thread." << std::endl;
								return;
							}



							dispatcher.TryEnqueue([this, selectedDevice]() {
								try {
									// Check if the device is already set
							
									if (currentDevice && currentDevice.Id() == selectedDevice.Id()) {
										std::cout << "Audio device is already set to the selected device." << std::endl;
										return;
									}



									// Set the selected device as the audio output device
									std::cout << "Setting mediaPlayer.AudioDevice() to " << winrt::to_string(selectedDevice.Id()) << std::endl;
									mediaPlayer.AudioDevice(selectedDevice);
									currentDevice = selectedDevice;
								}
								catch (const winrt::hresult_error& ex) {
									std::cerr << "Error setting AudioDevice: " << winrt::to_string(ex.message()) << std::endl;
								}
								catch (...) {
									std::cerr << "Unknown error when setting AudioDevice." << std::endl;
								}
								});
						}
						catch (const std::exception& ex) {
							std::cerr << "Exception in background thread: " << ex.what() << std::endl;
						}
						catch (...) {
							std::cerr << "Unknown exception in background thread." << std::endl;
						} })
					.detach();

				result->Success();
			}
			else
			{
				std::cerr << "Device ID not found in method arguments" << std::endl;
				result->Error("device_id_not_found", "Device ID not found in method arguments");
			}
		}
		else
		{
			result->NotImplemented();
		}
	}

	void loadSource(const flutter::EncodableMap& source) const&
	{
		auto items = mediaPlaybackList.Items();
		items.Clear(); // Always clear the list since we are resetting

		const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));

		if (type->compare("concatenating") == 0)
		{
			const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(source, "children"));

			for (auto& child : *children)
			{
				const auto* childMap = std::get_if<flutter::EncodableMap>(&child);
				auto item = createMediaPlaybackItem(*childMap);
				items.Append(item);
			}

			mediaPlayer.Source(mediaPlaybackList.as<Playback::IMediaPlaybackSource>());
		}
		else
		{
			mediaPlayer.Source(createMediaPlaybackItem(source).as<Playback::IMediaPlaybackSource>());
		}
	}

	/**
	 * Creates a single MediaPlaybackItem, which can be used directly or inside a list.
	 */
	Playback::MediaPlaybackItem createMediaPlaybackItem(const flutter::EncodableMap& source) const&
	{
		const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));

		if (type->compare("clipping") == 0)
		{
			const auto* child = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "child"));
			auto childSource = createMediaSource(*child);

			const auto* startUs = std::get_if<int32_t>(ValueOrNull(*child, "start"));
			const auto* endUs = std::get_if<int32_t>(ValueOrNull(*child, "end"));

			auto start = 0; // Default to 0
			if (startUs != nullptr)
			{
				start = *startUs;
			}

			if (endUs != nullptr)
			{
				// We have a duration limit
				auto duration = *endUs - start;

				return Playback::MediaPlaybackItem(
					childSource,
					TimeSpan(std::chrono::microseconds(start)),
					TimeSpan(std::chrono::microseconds(duration)));
			}
			else
			{
				return Playback::MediaPlaybackItem(
					childSource,
					TimeSpan(std::chrono::microseconds(start)));
			}
		}
		else
		{
			return Playback::MediaPlaybackItem(createMediaSource(source));
		}
	}

	/**
	 * Creates a single MediaSource.
	 */
	MediaSource createMediaSource(const flutter::EncodableMap& source) const&
	{
		const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
		if (type->compare("progressive") == 0 || type->compare("dash") == 0 || type->compare("hls") == 0)
		{
			const auto* uri = std::get_if<std::string>(ValueOrNull(source, "uri"));
			return MediaSource::CreateFromUri(
				Uri(TO_WIDESTRING(*uri)));
		}
		else
		{
			throw std::invalid_argument("Source is unsupported or can not be nested: " + *type);
		}
	}

	void broadcastState()
	{
		try
		{
			broadcastPlaybackEvent();
		}
		catch (winrt::hresult_error const& ex)
		{
			std::cerr << "[just_audio_windows] Broadcast event error: " << winrt::to_string(ex.message()) << std::endl;
		}

		try
		{
			broadcastDataEvent();
		}
		catch (winrt::hresult_error const& ex)
		{
			std::cerr << "[just_audio_windows] Broadcast data error: " << winrt::to_string(ex.message()) << std::endl;
		}
	}

	void broadcastPlaybackEvent()
	{
		auto session = mediaPlayer.PlaybackSession();

		auto eventData = flutter::EncodableMap();

		auto duration = TO_MICROSECONDS(session.NaturalDuration());

		auto now = std::chrono::system_clock::now();

		eventData[flutter::EncodableValue("processingState")] = flutter::EncodableValue(processingState(session.PlaybackState()));
		eventData[flutter::EncodableValue("updatePosition")] = flutter::EncodableValue(TO_MICROSECONDS(session.Position()));                 // int
		eventData[flutter::EncodableValue("updateTime")] = flutter::EncodableValue(TO_MILLISECONDS(now.time_since_epoch()));                 // int
		eventData[flutter::EncodableValue("bufferedPosition")] = flutter::EncodableValue((int64_t)(duration * session.BufferingProgress())); // int
		eventData[flutter::EncodableValue("duration")] = flutter::EncodableValue(duration);                                                  // int

		int64_t currentIndex = mediaPlaybackList.CurrentItemIndex();
		if (currentIndex != 4294967295)
		{                                                                                             // UINT32_MAX - 1
			eventData[flutter::EncodableValue("currentIndex")] = flutter::EncodableValue(currentIndex); // int
		}

		event_sink_->Success(eventData);
	}

	int processingState(Playback::MediaPlaybackState state)
	{
		auto session = mediaPlayer.PlaybackSession();

		if (state == Playback::MediaPlaybackState::None)
		{
			return 0; // idle
		}
		else if (state == Playback::MediaPlaybackState::Opening)
		{
			return 1; // loading
		}
		else if (state == Playback::MediaPlaybackState::Buffering)
		{
			return 2; // buffering
		}
		else if (session.Position().count() == session.NaturalDuration().count())
		{
			return 4; // completed
		}
		return 3; // ready
	}

	void broadcastDataEvent()
	{
		auto session = mediaPlayer.PlaybackSession();
		auto eventData = flutter::EncodableMap();

		auto isPlaying = session.PlaybackState() == Playback::MediaPlaybackState::Playing;

		eventData[flutter::EncodableValue("playing")] = flutter::EncodableValue(isPlaying);
		eventData[flutter::EncodableValue("volume")] = flutter::EncodableValue(mediaPlayer.Volume());
		eventData[flutter::EncodableValue("speed")] = flutter::EncodableValue(session.PlaybackRate());
		eventData[flutter::EncodableValue("loopMode")] = flutter::EncodableValue(getLoopMode());
		eventData[flutter::EncodableValue("shuffleMode")] = flutter::EncodableValue(getShuffleMode());

		data_sink_->Success(eventData);
	}

	int getLoopMode()
	{
		if (mediaPlayer.IsLoopingEnabled())
		{
			// one
			return 1;
		}
		else if (mediaPlaybackList.AutoRepeatEnabled())
		{
			// all
			return 2;
		}
		else
		{
			// pff
			return 0;
		}
	}

	int getShuffleMode()
	{
		// TODO(bdlukaa): playlists
		return 0;
	}

	flutter::EncodableMap collectIcyMetadata()
	{
		auto icyData = flutter::EncodableMap();

		// TODO: Icy Metadata
		// mediaPlayer.PlaybackMediaMarkers();

		return icyData;
	}

	/// Transforms a num into positive, if negative
	int negativeToPositive(int num)
	{
		if (num < 0)
		{
			return num * (-1);
		}
		return num;
	}

	void seekToItem(uint32_t index)
	{
		if (index >= mediaPlaybackList.Items().Size())
		{
			return;
		}

		try
		{
			mediaPlaybackList.MoveTo(index);
		}
		catch (winrt::hresult_error const& ex)
		{
			std::cerr << "[just_audio_windows] Failed to seek to item: " << winrt::to_string(ex.message()) << std::endl;
		}

		broadcastState();
	}

	void seekToPosition(int microseconds)
	{
		mediaPlayer.Position(TimeSpan(std::chrono::microseconds(microseconds)));

		broadcastState();
	}

	void setShuffleOrder(const flutter::EncodableMap& source)
	{
		const std::string* type = std::get_if<std::string>(ValueOrNull(source, "type"));
		// const std::string* id = std::get_if<std::string>(ValueOrNull(source, "id"));

		if (type->compare("concatenating") == 0)
		{
			const auto* shuffleOrder = std::get_if<flutter::EncodableList>(ValueOrNull(source, "shuffleOrder"));

			// A copy of mediaPlaybackList.Items()
			std::vector<Playback::MediaPlaybackItem> itemsCopy{};
			for (auto item : mediaPlaybackList.Items())
			{
				itemsCopy.push_back(item);
			}

			// then we apply the suffling to itemsCopy
			for (int i = 0; i < ((int)shuffleOrder->size()); i++)
			{
				auto item = itemsCopy.at(i);

				auto insertAt = (*shuffleOrder).at(i).LongValue();

				// delete the item at i
				itemsCopy.erase(itemsCopy.begin() + i);
				itemsCopy.insert(itemsCopy.begin() + insertAt, item);
			}

			// and finnaly provide it to the player list
			mediaPlaybackList.SetShuffledItems(itemsCopy);

			itemsCopy.clear();
			itemsCopy.shrink_to_fit();

			const auto* children = std::get_if<flutter::EncodableList>(ValueOrNull(source, "children"));
			for (auto child : *children)
			{
				setShuffleOrder(std::get<flutter::EncodableMap>(child));
			}
		}
		else if (type->compare("looping") == 0)
		{
			const flutter::EncodableMap* child = std::get_if<flutter::EncodableMap>(ValueOrNull(source, "child"));
			setShuffleOrder(*child);
		}
		else
		{
			// can not shuffle a single-audio media source
		}
	}
};
