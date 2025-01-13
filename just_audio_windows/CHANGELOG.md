## [0.2.6]

- [fix]: optimized setOutputDevice calls and abort the call if player is already disposed

## [0.2.5]

- [fix]: resolved issue that caused app to freeze during setOutputDevice function (now it runs in thread)

## [0.2.4]

- [new]: Added setOutputDevice functionality

## [0.2.0]

- [new]: Support playlists ([#8](https://github.com/bdlukaa/just_audio_windows/pull/8), [#9](https://github.com/bdlukaa/just_audio_windows/pull/9))
- [fix]: Fixed build in VS17.3 ([#10](https://github.com/bdlukaa/just_audio_windows/pull/10))

## 0.1.0

- [fix]: `seek` no longer throws error
- [new]: Added support for data event stream
- [new]: Support for looping over a single track

## 0.0.1

- Initial Base Implementation
  - `init`, `disposePlayer`, `disposeAllPlayers`
  - `load`, `play`, `pause`, `setVolume`, `setSpeed`, `setPitch`, `seek`
  - `dispose`
  - Error reporting
