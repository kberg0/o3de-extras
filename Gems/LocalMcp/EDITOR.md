# Editor authoring

This gem requires the companion O3DE engine branch
`codex/local-mcp-script-defined-multiplayer`. The native editor and Script Canvas
workflow has passed headless acceptance; source and binary provenance remain in
the accompanying review bundle.

The Editor uses the existing LocalMcp server, bridge and log ring on a separate,
disabled-by-default endpoint. Set `/O3DE/Gems/LocalMcp/Editor/Enabled=true` and
`/O3DE/Gems/LocalMcp/Editor/Port=4601`. It accepts local peers only. The runtime
module does not link or advertise editor tools, and builders do not activate it.
Script Canvas must be enabled in the project for its native authoring owner.

`binding_search` and `binding_describe` expose the native Automation bindings and
their actual signatures and availability. `binding_call`, `binding_get`,
`binding_set` and `binding_release` reuse EditorPythonBindings conversion and
ownership. They do not evaluate caller-supplied Python. A generic reflected call
does not imply native undo support; the first-class editing recipes use the
appropriate owner and undo stack.

## First-class operations

- Entity creation/deletion/reparenting, local/world transforms and component
  discovery/add/remove/property reads/writes use the prefab-aware editor owners.
- `level_create`, `level_open`, `level_save`, `prefab_create` and `prefab_save`
  operate on native documents. Save/discard decisions must be explicit; saving
  a level does not silently save other dirty prefab templates.
- `asset_import` copies a new source and observes real AP jobs and catalog
  products. Copies and saves remain on disk when a request fails or is canceled.
- `editor_play`, `editor_stop`, `editor_state`, `editor_undo` and `editor_redo`
  use the existing editor state and undo owners.

Document replacement, disk saves and play transitions do not create editor undo
batches. Play completes when native game mode starts; prefab propagation pauses
during play and resumes before stop completes.

## Script Canvas

| Tools | Inputs |
| --- | --- |
| `sc_create`, `sc_open` | Absolute project `.scriptcanvas` source `path` |
| `sc_describe` | `graph` |
| `sc_palette` | Optional `query`, `after`, `limit` (1–256) |
| `sc_add_node` | `graph`, discovered `palette`, tagged Vector2 `position` |
| `sc_connect` | `graph`, `source_node`, `source_slot`, `target_node`, `target_slot` |
| `sc_set_value` | `graph`, `node`, `slot`, typed `value` |
| `sc_set_variable` | `graph`, existing variable UUID `variable`, typed `value` |
| `sc_remove_nodes`, `sc_remove_connections` | `graph`, discovered `nodes`/`connections` IDs |
| `sc_save` | `graph`, absolute project source `path` |
| `sc_reload` | `graph`, optional explicit `discard` |
| `sc_undo`, `sc_redo` | `graph`; these use the Script Canvas undo stack |
| `sc_attach`, `sc_attachment` | Valid editor `component` pair; attach also takes source `path` |

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
Borrowed strings from the native marshaller are copied into owned strings;
numeric inputs retain their exact native type.

`sc_save` observes the real FileSaver callback, then reuses the asset importer’s
AP job/catalog observer. It returns actual processing products in
`assetProcessing`; a saved file alone does not imply runtime readiness. Disk
saves are not undoable. A disconnect cancels observation, not an already issued
native save. Calls remain pending between main-thread polls and never retry a
mutation after a timeout.

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

The full headless proof covers native graph authoring, undo/redo, save and resave with actual AP products, reload, component attachment, persisted level readback and the resaved graph executing in play. It uses null RHI and offscreen Qt; rendered layout is not covered. Teardown closes the owned process job.

The engine branch includes the combined multiplayer headless acceptance driver at
`Gems/Multiplayer/Multiplayer_ScriptCanvas/Code/Tests/Headless/README.md`.
Pass this external gem location to its project preparer with `--local-mcp-gem`.
