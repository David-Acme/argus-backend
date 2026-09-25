# argus_clients_vlm

The argus-vlm wire, caller side. Two transports answer the same caption: the
gRPC client `argus::vlm::Client` for `argus.vlm.v1` (the contract
`packages/contracts/vlm` owns) and the transitional HTTP client
`VlmHttpClient`, which posts `{image_b64, prompt, camera_id}` to
`/vlm/v1/describe` with a Drogon `HttpClient`, base64-encodes the JPEG itself
and reads the caption from the `{status, info}` envelope. The façade
`VlmClient` is what a consumer holds: it takes the gRPC path when
`vlm.grpc_target` is set and the HTTP path otherwise.

The package exists so argus-guard does not hand-roll either wire (rule 23), and
so the stub, the credential header and the deadline live in one place (rule
27). It is built through `add_subdirectory` by its host, like
`argus::clients::llm`; it has no process, listener or configuration of its own
beyond the two gRPC knobs the façade reads.

## Why the façade answers nullopt on the gRPC leg

Guard reads "no caption" from an empty optional and carries on with the
assessment; it never catches around `describe`. The façade therefore folds
every `ResponseException` the gRPC call raises — a refusal, a deadline, an
unreachable server — into `std::nullopt`, so turning the gRPC knob on cannot
turn a vision outage into a failed assessment. The HTTP leg keeps its older
shape, where a transport failure raises `drogon::HttpException` out of
`sync_wait`; that asymmetry predates this package's gRPC leg and is pinned by
the suite rather than silently changed under guard.

## Why the gRPC call runs in BlockingTask

`argus::vlm::Client::describe` is a blocking unary call, and guard awaits the
façade on its Drogon event loop. Rule 13c forbids a blocking call there, so the
façade hands the call to `BlockingTask`, which runs it on its own thread and
resumes the coroutine on the app loop.

## Consumers

- argus-guard `GuardAssessment` — describes the person crop fetched through
  `argus.camera.v1.CameraActionService.GetPersonCrop` before the optional
  LLM threat classification.
- argus-vlm itself — `argus_vlm-rpc` links the package PUBLIC for the
  `argus::vlm::Capabilities` type its server's input names, and its
  `vlm-rpc-test` drives the server through the gRPC client and the façade.
