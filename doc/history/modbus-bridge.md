# The Modbus bridge - history

How the Modbus bridge came about. What it is and does is in
[A Modbus bridge](../modbus-bridge.md). Newest first.

## 2026-10-01 - v1 built

Modbus TCP, the address in the endpoint and how to read it in `channelInfo`, change-only reporting
with a deadband, queued writes, `modbusStatus` on a device that stops answering - the plugin in its
own repo, corModbusBridge, tested against a Modbus device in the functests.

## Channel parameters: from the endpoint string to `channelInfo`

A Channel had an endpoint, a kind and a direction. Modbus needs typing and scaling; the first
version put them in the endpoint string, which made a scale factor part of the Channel's identity,
and left every future transport (OPC-UA sampling, MQTT QoS and codec) to invent its own URL syntax.
The endpoint became the address and `channelInfo` - key-value pairs, as `receiverInfo` - the rest,
handed to the plugin by `channelAddInfo` (bridge ABI 9).

This settled the first of the design's open questions - "Mapping in the endpoint, or ...?" -
**decided: `channelInfo`**.

## Before any code - the proposal

`modbus-bridge.md` was originally written as a proposal, to decide the shape before any code.
