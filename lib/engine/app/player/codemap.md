# lib/engine/app/player

## Responsibility
The `engine.app:player` module provides the player service and graph-based state machine that maps input devices to player identities. It enables multiple players (keyboard, mouse, multiple gamepads) to coexist, each with their own input bindings and action mappings. The player graph tracks device-to-player mappings, handles hot-plugging of gamepads, and provides callbacks when players are added or removed.

## Design
- **IPlayerService** interface (in `engine.app:service.player`) — provides `getPlayer()`, `enumeratePlayers()`, `getOrCreateKeyboardPlayer()`, `addGamepadPlayer()`, `removePlayer()`, and `whenPlayerAdded/Removed` callbacks
- **PlayerIdentity** — holds `PlayerId` (Numeric<u64, class Player>), `InputDeviceID`, `local_index`, and `EPlayerKind` (keyboard/gamepad). Used for device-to-player mapping.
- **Player** class (in `engine.app:input.player`) — holds `PlayerIdentity`, `InputListener` (per-player keybindings), `StableVectorInplace<SharedInputDevice>` for device views, and `StableVector<InputFrameSnapshot>` for accumulated messages. Methods: `pushDeviceView()`, `getActionValue()`, `addMapping()`, `clearFrameMessages()`, `pushFrameMessage()`, `sample()`.
- **PlayerGraph** (in `engine.app:player.graph`) — `FlatMap<PlayerId, unique_ptr<Player>>` for player storage; `FlatMap<InputDeviceID, PlayerId>` for device→player reverse mapping. Methods: `getPlayer()`, `enumeratePlayers()`, `findPlayerForDevice()`, `getOrCreateKeyboardPlayer()`, `addGamepadPlayer()`, `removePlayer()`, `whenPlayerAdded/Removed`, `clear()`.
- Player IDs are generated via `std::mt19937_64` random number generator (in `GlfwPlayer`).
- `EPlayerKind` distinguishes keyboard (`0`) from gamepad (`1`) players.
- `getOrCreateKeyboardPlayer()` checks if a player already exists for the keyboard device; if not, generates a new `PlayerId` and creates one via `PlayerGraph::getOrCreateKeyboardPlayer()`.
- `addGamepadPlayer()` similar pattern for gamepads, also stores controller index in player identity.
- Callbacks `whenPlayerAdded`/`whenPlayerRemoved` are `Callback<std::error_code(const IPlayerService&, const Player&)>` — consumers register these to be notified of player lifecycle events.

## Flow
1. Application startup: `GlfwPlatform::initialize()` creates `GlfwPlayer::get()`, which creates an initial keyboard player for the default keyboard device
2. Gamepad hot-plug: `GlfwInput::pollGamepads_()` detects new joystick → calls `GlfwPlayer::addGamepadPlayer(controller_index)` → `PlayerGraph::addGamepadPlayer()` → creates new `Player` with gamepad device, stores device→player mapping, fires `whenPlayerAdded` callback
3. Player removal: `GlfwInput::pollGamepads_()` detects joystick removal → `GlfwPlayer::removePlayer(id)` → `PlayerGraph::removePlayer()` → erases player and device mappings, fires `whenPlayerRemoved` callback
4. Per-frame input: `Application::update()` → `m_cached_input_service->postInputMessages(dt)` → routes through `GlfwInput` → `routeMessage_()` → looks up player via `m_graph.findPlayerForDevice(device_id)` → delivers to that player's `InputListener`
5. Per-action value: `Player::getActionValue(action)` → `m_listener.getActionValue(action)` → resolves the action value from the per-player keybindings
6. Player enumeration: `IPlayerService::enumeratePlayers()` → `GlfwPlayer::enumeratePlayers()` → `PlayerGraph::enumeratePlayers()`

## Integration
- **Consumers**: `GlfwInput` (uses `m_player_service` to add/remove players), `ImGuiService` (may register per-player keybindings), gameplay systems that query `IPlayerService::getPlayer()` or `IPlayerService::whenPlayerAdded`
- **Depends on**: `engine.core` (safe_ptr, safe_object, IService), `engine.math`, `std` (mt19937_64, function_ref), `engine.app:input.player` (Player, PlayerIdentity, PlayerGraph), `engine.app:player.graph` (PlayerGraph), `engine.app:input.device` (KeyboardDevice, MouseDevice, GamepadDevice), `engine.app:input.listener` (InputListener), `engine.app:input.mapping` (InputMapping)
- **Provides**: `engine.app:input.player` module namespace with `Player`, `PlayerIdentity`, `InputFrameSnapshot`, `SharedPlayer`; `engine.app:player.graph` module namespace with `PlayerGraph`; `engine.app:service.player` module namespace with `IPlayerService`
- **Used by**: `GlfwPlayer` (primary consumer), `Application` (via `m_cached_player_service` or services store), gameplay code that needs per-player action values

## Key Files
- `App.Player.cppm` — `EPlayerKind`, `PlayerIdentity`, `InputFrameSnapshot`, `Player` class declaration, `SharedPlayer` typedef
- `App.Player.cpp` — Player method implementations (constructor, pushDeviceView, getActionValue, addMapping, clearFrameMessages, pushFrameMessage, sample)
- `App.Player.Graph.cppm` — `PlayerGraph` class declaration (FlatMap<PlayerId, unique_ptr<Player>>, FlatMap<InputDeviceID, PlayerId>)
- `App.Player.Graph.cpp` — PlayerGraph method implementations (getPlayer, enumeratePlayers, findPlayerForDevice, getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, clear)
- `App.Player.Graph.cppm` also re-exports `IPlayerService`-related types
- `App.Service.Player.cppm` — `IPlayerService` interface declaration (getPlayer, enumeratePlayers, getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, PlayerCallback)