# lib/engine/app/player

## Responsibility
The `engine.app:player` module provides the player service and graph-based state machine that maps input devices to player identities. It enables multiple players (unified keyboard+mouse, multiple gamepads) to coexist, each with its own input listener and key-to-action bindings. The player graph tracks device-to-player mappings and provides callbacks when players are added or removed.

## Design
- **IPlayerService** interface (in `engine.app:service.player`) — provides `getPlayer()`, `enumeratePlayers()`, `getOrCreateKeyboardPlayer()`, `addGamepadPlayer()`, `removePlayer()`, and `whenPlayerAdded/Removed` callbacks
- **PlayerIdentity** — holds `PlayerId` (`Numeric<u64, Player>`), `InputDeviceID`, `m_local_index`, and `EPlayerKind` (`keyboard`/`gamepad`); three-way comparison orders by kind, then local index, then user id
- **Player** class (in `engine.app:player`) — holds `PlayerIdentity`, an `InputListener` (per-player keybindings), `StableVectorInplace<SharedInputDevice>` device views, and a `StableVector<InputMessage>` frame-message buffer. Methods: `pushDeviceView()`, `getActionValue()`, `addMapping()`, `clearFrameMessages()`, `pushFrameMessage()`, `sample()` (→ `InputFrameSnapshot` with identity + message copy)
- **PlayerGraph** (in `engine.app:player.graph`) — `FlatMap<PlayerId, unique_ptr<Player>>` player storage plus `FlatMap<InputDeviceID, PlayerId>` device→player reverse map; `IPlayerService::PlayerCallback` add/remove notifiers. Methods return `Expected<SharedPlayer>` (creation) or `std::error_code` (removal)
- Keyboard players bind **both** keyboard and mouse: `getOrCreateKeyboardPlayer(service, user_id, KeyboardDevice&, MouseDevice&)` pushes both device views into the one player; gamepad players bind a single device via `addGamepadPlayer(service, user_id, GamepadDevice&)`
- PlayerIds are minted by the backend (`GlfwPlayer`) and passed in; the graph itself performs no RNG
- Callbacks `whenPlayerAdded`/`whenPlayerRemoved` are `Callback<std::error_code(const IPlayerService&, const Player&)>` — consumers register these to be notified of player lifecycle events

## Flow
1. Application startup: `GlfwPlatform::initialize()` creates `GlfwPlayer::get()`, which lazily creates the keyboard(+mouse) player for the default devices on first access
2. Gamepad hot-plug: `GlfwInput` detects a new joystick → calls into `GlfwPlayer::addGamepadPlayer(controller_index)` → `PlayerGraph::addGamepadPlayer()` → creates a new `Player` bound to that `GamepadDevice`, stores the device→player mapping, fires the `whenPlayerAdded` callback
3. Gamepad removal: joystick loss → `GlfwPlayer::removePlayer(id)` → `PlayerGraph::removePlayer()` → erases the player and its device mappings, fires the `whenPlayerRemoved` callback
4. Per-frame input: `Application::update()` → input poll → messages route through the `InputContext` tree → each player's `InputListener` resolves its own keybindings into per-action values
5. Per-action value: `Player::getActionValue(action)` → `m_listener.getActionValue(action)` → resolves the action value from the per-player mappings
6. Player enumeration: `IPlayerService::enumeratePlayers()` → `GlfwPlayer::enumeratePlayers()` → `PlayerGraph::enumeratePlayers()`

## Integration
- **Consumers**: `GlfwInput` (device connect/disconnect drives player add/remove), gameplay systems that query `IPlayerService::getPlayer()` or subscribe via `whenPlayerAdded/Removed`
- **Depends on**: `engine.core` (safe_ptr, safe_object, IService, FlatMap, Collector/Expected), `std` (error_code, function_ref), `engine.app:input.device` (unified `KeyboardDevice`/`MouseDevice`/`GamepadDevice`, `InputMessage`, `SharedInputDevice` — no `device/` sub-partition remains), `engine.app:input.listener` (`InputListener`, `SharedInputMapping`), `engine.app:input.action` (`InputAction`, `InputMapping` — mapping types live here since `App.Input.Mapping` was removed), `engine.app:input.key`
- **Provides**: `engine.app:player` module namespace with `Player`, `PlayerIdentity`, `InputFrameSnapshot`, `SharedPlayer`; `engine.app:player.graph` module namespace with `PlayerGraph`; `engine.app:service.player` module namespace with `IPlayerService`
- **Used by**: `GlfwPlayer` (primary consumer, owns the graph), `Application` (via services store), gameplay code that needs per-player action values

## Key Files
- `App.Player.cppm` — `EPlayerKind`, `PlayerId`, `PlayerIdentity`, `InputFrameSnapshot`, `Player` class declaration, `SharedPlayer` typedef
- `App.Player.cpp` — Player method implementations (constructor, pushDeviceView, getActionValue, addMapping, clearFrameMessages, pushFrameMessage, sample)
- `App.Player.Graph.cppm` — `PlayerGraph` class declaration (player map, device→player map, add/remove callbacks)
- `App.Player.Graph.cpp` — PlayerGraph method implementations (getPlayer, enumeratePlayers, findPlayerForDevice, getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, clear)
- `App.Service.Player.cppm` — `IPlayerService` interface declaration (getPlayer, enumeratePlayers, getOrCreateKeyboardPlayer, addGamepadPlayer, removePlayer, whenPlayerAdded/Removed, PlayerCallback)
