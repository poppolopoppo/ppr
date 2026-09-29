# lib/engine/physics/

## Responsibility

`engine.physics` is the renderer-independent Box2D integration boundary. Scene
owns the disposable world, entity-keyed body/sensor runtime, and an opaque
serial-keyed static-body chain API. Coupling publishes sorted sensor and contact
events, body states and observed awake transitions through caller-owned buffers.

## Seams

- `:scene` — opaque, RAII-owned world with fixed-step configuration, body and sensor
  creation, sensor/contact-event draining, sleep controls and generation-safe handles.
- `:chunk_colliders` — accepts caller-owned row-major element-ID spans (nonzero
  means solid, missing neighbors mean empty), dirty-position deltas, and active
  interest; rebuilds bounded row-major slices. Every edited chunk invalidates
  itself and supplied cardinal neighbors. Chains are staged before old handles
  are removed, so a failed build retains the previous chunk collider. Outward
  normals use counterclockwise solid-left edges; neighboring chains provide ghost
  vertices at seams. Query materialization is explicit; quiescent chunks have
  no chains. The caller must retain element spans during each call and resubmit
  a dirty position only for a new edit.
- `:coupling` — caller-supplied sorted entity handles provide canonical body/wake
  order; sensor events sort by sensor entity, visitor entity, kind, then begin/end.
  Contacts order by ascending normalized entity pair (zero denotes the Scene-owned
  chunk chain), then end/begin/hit, then contact point, normal and approach speed.
  A nonempty contact destination drains the last step once; short buffers fail
  without consuming contacts. Bounded force/torque application and boundary
  velocity sampling are reserved unsupported seams.

## Dependencies

`engine.core` and `engine.math` are public target dependencies; vcpkg `box2d`
3.1.1 (`box2d::box2d`) is private. Box2D headers and IDs must stay in `.cpp`
files, never in exported partitions. `engine.physics` and `engine.sim` are
siblings and neither imports the other.
