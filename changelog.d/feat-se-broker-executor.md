### New

- Service Execution: coraine as another broker's Service Executor - `POST /ngsi-ld/ex/v1/executor/{serviceName}` and `DELETE /ngsi-ld/ex/v1/executor/{serviceName}/{id}` take the requests a broker sends an HTTP executor; the service is executed as an invocation on coraine's own entity, and an asynchronous execution's every change is reported to the broker that asked with `PATCH <reportTo>/ngsi-ld/v1/services/{id}` (`?reportTo=` in the endpoint). Two coraines chain their services this way (doc/service-execution.md, "Another broker as the executor").
- Service Execution: a registration's endpoint may be `cor://` - the executor's requests over the cor:// transport; and an execution's `notification.endpoint` may be `cor://`.

### Fixed

- Service Execution: once a service is executed over http(s):// or cor://, invoking a service (`POST /ngsi-ld/v1/entities/{id}/services/{name}`) no longer runs on the I/O thread that read it - it may wait for the network, and an executor that called back on that thread (an HTTP executor's PATCH report) waited for it until the call timed out. With bridge executors only, invocations still run inline.
- Service Execution: a cancel sent to an HTTP executor whose endpoint has a query puts the execution id in the path, before the query.
