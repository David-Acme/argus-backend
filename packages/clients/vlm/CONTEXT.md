# argus_clients_vlm

The argus-vlm internal wire, caller side. `VlmClient::describe` posts
`{image_b64, prompt, camera_id}` to `/vlm/v1/describe` with a Drogon
`HttpClient`, base64-encodes the JPEG itself and returns the caption from the
`{status, info}` envelope; nullopt covers status and empty-caption failures,
while a transport failure raises `drogon::HttpException` out of `sync_wait`
rather than answering nullopt.

The package exists so argus-guard does not hand-roll the wire (rule 23). It is
built through `add_subdirectory` by its host, like `argus::clients::llm`; it
has no process, listener or configuration of its own.

## Consumers

- argus-guard `GuardAssessment` — describes the person crop fetched through
  `argus.camera.v1.CameraActionService.GetPersonCrop` before the optional
  LLM threat classification.
