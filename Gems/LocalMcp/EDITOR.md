# Editor authoring

The editor endpoint needs these O3DE engine changes: editor-component-prefab-automation,
editor-level-python-automation, editorpythonbindings-structured-calls and
scriptcanvas-graph-automation.

The Editor uses the existing LocalMcp server, bridge and log ring on a separate,
disabled-by-default endpoint. Set `/O3DE/Gems/LocalMcp/Editor/Enabled=true` and
`/O3DE/Gems/LocalMcp/Editor/Port=4601`. It accepts local peers only. The runtime
module does not link or advertise editor tools, and builders do not activate it.
Script Canvas must be enabled in the project for its native authoring owner.

Tool arguments are advertised in `Code/Source/Editor/AutoGen/LocalMcpEditorTools.McpTools.xml`
and `LocalMcpBindingTools.McpTools.xml`, which AzAutoGen turns into both the `tools/list` schemas
and the argument checks. Editor tools refuse arguments their schema does not list.

## Reflected bindings

Every BehaviorContext entry is reachable through the binding tools, which reuse
EditorPythonBindings conversion and ownership. A generic reflected call does not
imply native undo support; the editor tools below use the appropriate owner and
undo stack.

- `binding_search` ranks bindings by name, alias and tooltip, including unavailable
  entries and the reason. It filters by `kind` (`Class`, `Method`, `Property`, `Bus`,
  `Event` or `BusProperty`) and by `module`, which also matches its dotted sub-modules.
- `binding_browse` walks the editor automation bindings as azlmbr groups them:
  modules, then a module's classes, buses and global members, then a class's or bus's
  members. `path` is the dotted path of the level to list; empty lists the top-level
  modules. Each child reports `name`, `path`, `kind`, `childCount`, its `tooltip` and,
  for a binding, its `binding` ID. Pages take `limit` (1–100) and `after`, like
  `binding_search`. A class or bus without a `Module` attribute is listed under
  azlmbr's fallback module: `default` for classes, `bus` for buses and `globals` for
  global methods and properties.
- `binding_describe` reports native signatures, defaults, types, scope and senders.
- `binding_call`, `binding_get`, `binding_set` and `binding_release` call, read,
  write and release.

### Module tools

`binding_open_module` adds one typed tool to `tools/list` for each callable
automation binding in a module and its sub-modules; `binding_close_module` removes
them again. A call of a typed tool is a `binding_call` of its binding, with the same
checks and replies.

- The tool name is the module path, the owning class or bus, and the member, such as
  `scriptcanvas.automation.GraphAutomationRequestBus.AddNode`. A path with characters
  outside `[A-Za-z0-9_.-]`, longer than 128 characters, or whose name another open tool
  already has, is cut and ends in `-` and a CRC32 of the binding ID instead. A binding
  whose hashed name also clashes is reported in `skipped`; call it with `binding_call`.
- `inputSchema` has one property per native argument, typed from the reflected
  parameter: `boolean`, `integer`, `number` or `string`, or a tagged value as
  `binding_describe` shows it. Instance methods take `target`, an object handle, and
  addressed bus events take `address`, required when the bus has no broadcast sender.
  Arguments are positional, so only trailing arguments with defaults may be omitted.
  An overloaded binding always calls its first overload.
- The description is the binding's tooltip followed by its binding ID.
- Properties, constructors, events without a synchronous sender and bindings with an
  unconvertible parameter get no tool; the last are listed in `skipped` with the reason.
- At most 128 generated tools are open at once. Opening a module that would exceed it
  fails, naming the counts; open a sub-module or close another module. A module that
  overlaps an open one must be closed first.

The open modules are global to the endpoint, not per client: the server has no MCP
sessions, and every client sees the same tools. The call that opens or closes a
module is answered with `notifications/tools/list_changed` ahead of its reply when the
client accepts `text/event-stream` (see [README.md](README.md)); other clients see the
change on their next `tools/list`.

## Editor tools

These tools add something a binding call cannot: an editor state guard, an undo
batch, or a wait for prefab propagation, play mode, a level load, the Asset
Processor or a rendered frame.

| Tool | Why it is not a binding call |
| --- | --- |
| `entity_create`, `entity_delete`, `entity_set_parent`, `prefab_create` | Undo batch, ready-level guard and wait for prefab propagation; `entity_create` defaults to the focused prefab, `entity_set_parent` refuses cycles. |
| `transform_set` | Undo batch and `AddDirtyEntity`, so the change is recorded and saved. |
| `component_add`, `component_remove`, `component_set` | Undo batch around the native composition and property-tree calls, plus the propagation wait. |
| `level_create`, `level_open` | Explicit `unsaved` choice instead of a modal prompt, then a wait until the level is ready. |
| `level_save`, `prefab_save` | Refused during play and pending propagation; dirty prefab templates are reported, never saved silently. |
| `asset_import` | Copies a new source and waits for the Asset Processor jobs and catalog products. |
| `editor_play`, `editor_stop` | Wait for the actual mode transition. |
| `editor_undo`, `editor_redo` | Use the editor's undo stack, then wait for propagation. |
| `editor_state` | One reply with document, play, prefab focus, dirty templates, pending propagation and whether a tool is running. |
| `editor_info` | Endpoint readiness and the Qt platform. |
| `python_execute` | Runs Python; see below. |
| `screenshot` | Captures pixels; see below. |

Reads that were separate tools are binding calls: transforms through
`bus/TransformBus/event/GetLocalTM` and `GetWorldTM`, component types through
`bus/EditorComponentAPIBus/event/BuildComponentTypeNameListByEntityType` and
`FindComponentTypeIdsByEntityType`, attached components through `GetAllComponents`,
property values through `GetComponentProperty`, and property paths through
`BuildComponentPropertyTreeEditor`, `class/PropertyTreeEditor/method/BuildPathsListWithTypes`
and `binding_release`.

Document replacement, disk saves and play transitions do not create editor undo
batches. Play completes when native game mode starts; prefab propagation pauses
during play and resumes before stop completes. Copies and saves remain on disk when
a request fails or is canceled.

### python_execute

`python_execute` runs `code` (at most 32768 characters) as a script in the editor's
EditorPythonBindings interpreter, on the main thread, in a fresh namespace with
`azlmbr` importable. It returns `stdout`, `stderr`, the `traceback` of an uncaught
exception, `timedOut` and `truncated`; each text is cut at 65536 characters.
`timeout_ms` stops the code at its next Python line once it passes and defaults to,
and may not exceed, the connection deadline less 250 ms. A long native call is not
interrupted.

It is disabled by default and absent from `tools/list` until the editor's local
settings set `/O3DE/Gems/LocalMcp/Editor/PythonExecute=true`. The setting is read once
when the endpoint starts, so a client cannot enable it through `console_execute`.
Python has the editor's full access to the machine; enable it only for trusted local
agents.

### screenshot

`screenshot` returns a PNG as an MCP image content item after the JSON text, which
reports `target`, `width` and `height`. The longest side is at most 2048 pixels and
the PNG at most 8 MiB.

- `target=viewport` captures the default viewport through the Atom frame capture
  system after three frames. `position` and `look_at`, tagged Vector3 values given
  together, move the editor camera first. An editor without a renderer, such as one
  running with the null RHI, fails with an error that suggests `window`.
- `target=panel` grabs a view pane by its name, such as `Entity Inspector`, opening it
  if it is closed. An unknown name fails and lists the names in `panels`.
- `target=window` grabs the whole main window.

## Script Canvas

Script Canvas graphs are authored through the reflected
`GraphAutomationRequestBus` in module `scriptcanvas.automation`, either with
`binding_call` (`bus/GraphAutomationRequestBus/event/AddNode`) or as typed tools after
`binding_open_module` with `scriptcanvas.automation`. Its events are `Create`,
`Open`, `Reload`, `Describe`, `Palette`, `AddNode`, `Connect`, `SetValue`,
`SetVariable`, `RemoveNodes`, `RemoveConnections`, `Save`, `PollSave`,
`CancelSave`, `Attach`, `Attachment`, `Undo` and `Redo`.

Graph/node/connection IDs are decimal strings in the Script Canvas document
domain. They are not tagged editor EntityIds. Slot/variable IDs are native UUIDs.
Rediscover IDs after closing/reloading a document. Describe reports Script Canvas
and GraphCanvas mappings, slot types/directions and literal values.
Each slot includes `graphCanvasNode` and `graphCanvasSlot`. Add returns
`createdGraphCanvas`, which identifies the event child when the native node is a
handler wrapper. Use those mappings to distinguish individual receiver events.

Palette IDs include the native factory choice (`tree/path#choice`). Discover the
ID and inspect `factory` before adding a node. Multi-choice entries expose the
existing separate sender/receiver or get/set/change MIME factories; automation
never executes their interactive choice menu. Palette IDs expire when its native
model changes.

Datum writes preserve native types and notifications and refuse mismatched types,
connected inputs and variable-reference slots. Connect uses native endpoint
validation and GraphCanvas creation. Graph changes use Script Canvas's own undo
batch together with the editor batch. No `.scriptcanvas` JSON is patched directly.

`Save` starts the native FileSaver; call `PollSave` until it reports the result, or
`CancelSave`. A saved file alone does not imply that the Asset Processor has built it.
Disk saves are not undoable.

## Exposing a gem to agents

Any gem can make its editor operations available here without LocalMcp changes.
Reflect the class or bus to the BehaviorContext with
`AZ::Script::Attributes::Scope` set to `ScopeFlags::Automation`, a
`AZ::Script::Attributes::Module` such as `"mygem.tools"`, and a
`AZ::Script::Attributes::ToolTip` on the owner and on each method or event. Name the
arguments and give their tooltips through `BehaviorParameterOverrides`, and prefer
parameter types the binding tools convert directly: booleans, numbers, strings,
math types, `AZ::EntityId` and `AZ::Uuid`.

The binding then appears in `binding_browse` under its module and in
`binding_search`, and `binding_open_module` with that module turns each method or
event into a typed tool whose description is the tooltip. The same reflection makes
it available to editor Python as `azlmbr.mygem.tools`. Keep modules small enough to
open within the 128-tool limit, and keep operations that need undo or a wait in
native code that the binding calls.

## Supporting engine changes to review

The engine review commit contains the required native owner APIs. The table
below identifies their responsibilities; the LocalMcp implementation stays in
o3de-extras. Generic binding invocation reuses EditorPythonBindings
`PythonUtility` and its existing marshaller.

| Owner | Reason for the supporting change |
| --- | --- |
| `Code/Editor/CryEditPy.cpp` | Expose existing document state/create/open operations without unattended prompts. |
| `Code/Editor/Lib/Tests/test_CryEditPythonBindings.cpp` | Verify those owner bindings. |
| `AzToolsFramework/Component/EditorComponentAPI{Bus.h,Component.h,Component.cpp}` | Enumerate actual attached component instances, including pending/disabled components; use the matching wrapped instance and type when building a property tree. |
| `AzToolsFramework/Prefab/Instance/InstanceUpdateExecutor{.h,Interface.h}` | Observe queued prefab propagation before reporting completion. |
| `AzToolsFramework/Prefab/PrefabFocusHandler.cpp` | Reflect the existing `GetFocusedPrefabContainerEntityId` query used by `editor_state`. |
| `AzToolsFramework/Prefab/PrefabPublicRequest{Bus.h,Handler.h,Handler.cpp}` | Expose existing prefab creation/save/state and reflect its native path collection. |
| `AzToolsFramework/Prefab/PrefabPublicHandler.cpp` | Resolve absolute save/dirty-query paths to the loaded template; keep an explicit save destination through native `SaveTemplateToFile`, including new sources below asset scan folders. |
| `AzToolsFramework/Tests/Prefab/{PrefabAutomationTests.cpp,PrefabUpdateInstancesTests.cpp}` and test file list | Cover owner contracts and propagation. |
| `AzToolsFramework/PropertyTreeEditor/{PropertyTreeEditor.cpp,PropertyTreeEditorComponent.cpp}` and `Tests/PropertyTreeEditorTests.cpp` | Reflect the existing native property type query; copy borrowed native strings for string fields; verify reflection and ownership. |
| `ScriptCanvas/Code/Editor/GraphAutomation{.h,.cpp,Graph.cpp,Attachment.cpp}` | Small native adapter for real graph documents, palette factories, typed edits, undo and attachment. |
| `ScriptCanvas/Code/Editor/Include/ScriptCanvas/Bus/GraphAutomationRequestBus.h` | Automation-only reflected owner contract. |
| `ScriptCanvas/Code/Editor/Include/ScriptCanvas/Components/EditorScriptCanvasComponent.h` | Read back the actual selected source. |
| `ScriptCanvas/Code/Editor/SystemComponent.{h,cpp}` | Own/register/deactivate the editor adapter. |
| `ScriptCanvas/Code/Editor/View/Windows/MainWindow.{h,cpp}` | Delegate to the existing document/save owner; report automation save errors without a dialog; accept an empty offscreen clipboard when refreshing Paste. |
| `ScriptCanvas/Code/Tests/Editor/GraphAutomationTests.cpp` and editor source/test lists | Compile and verify the new owner contract and menu-free factory ownership. |

`AzToolsFramework` paths above are under `Code/Framework/AzToolsFramework/`;
`ScriptCanvas` paths are under `Gems/`. These phases do not change AzCore or
AzNetworking, add an interpreter, or add a third-party library.

