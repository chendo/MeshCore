# `MyMesh::advert()` can only send zero-hop, and the flood path is unreachable

**Severity:** low, but it blocks any UI that wants to offer both.

**Bucket:** 2 (seam). One overload in `examples/companion_radio/MyMesh.{h,cpp}`,
backward compatible.

## What happens

`MyMesh::advert()` is the only public way to send a self-advert:

```cpp
bool MyMesh::advert() {
  mesh::Packet* pkt = createSelfAdvert(...);
  if (pkt) { sendZeroHop(pkt); return true; }
  return false;
}
```

`CMD_SEND_SELF_ADVERT` can do both -- its optional byte selects flood or
zero-hop, and the flood branch calls `sendFloodScoped(default_scope, pkt, 0)`.
But `createSelfAdvert` and `sendFloodScoped` are both **protected**, so nothing
outside `MyMesh` can flood an advert. A device UI can therefore only offer the
zero-hop half of a feature the protocol already exposes to the phone.

Zero-hop and flood are not variations of one action: zero-hop reaches whoever
can hear this radio, flood crosses the mesh. A handheld wants both, and wants
them on different gestures.

## Fix

```cpp
bool advert(bool flood);
bool advert() { return advert(false); }   // unchanged for existing callers
```

with the body taking the same route `CMD_SEND_SELF_ADVERT` already takes:

```cpp
if (flood) {
  TransportKey default_scope;
  memcpy(&default_scope.key, _prefs.default_scope_key, sizeof(default_scope.key));
  sendFloodScoped(default_scope, pkt, 0);
} else {
  sendZeroHop(pkt);
}
```

No existing caller changes behaviour: `advert()` still means zero-hop.

## Why not do it downstream

There is no seam. Subclassing `MyMesh` to reach the protected members would mean
compiling a second mesh class alongside it, and `examples/companion_radio` and
`examples/simple_repeater` already collide on the names `MyMesh` and
`NodePrefs`, so that is a bigger change than the overload.
