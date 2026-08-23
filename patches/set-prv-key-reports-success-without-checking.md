# `set prv.key` reports success without checking that the key was stored

Status: not fixed in this fork. `src/helpers/CommonCLI.cpp` is unchanged.
Base: upstream `v1.17.1` (`d9296435`).
Kind: bug report, plus a fix small enough to send as one pull request.

## Summary

`set prv.key <hex>` always answers `OK, reboot to apply! New pubkey: <hex>`.
The reply does not depend on whether anything reached the flash. The command
never reads the key back, and the two layers under it discard every result they
have. So the one command in MeshCore that overwrites a keypair is also the one
command that cannot tell you it failed. You learn at the next boot, when the
node comes up under a key you did not choose.

## Where it is

`src/helpers/CommonCLI.cpp:523` handles the command:

```cpp
} else if (memcmp(config, "prv.key ", 8) == 0) {
  uint8_t prv_key[PRV_KEY_SIZE];
  bool success = mesh::Utils::fromHex(prv_key, PRV_KEY_SIZE, &config[8]);
  // only allow rekey if key is valid
  if (success && mesh::LocalIdentity::validatePrivateKey(prv_key)) {
    mesh::LocalIdentity new_id;
    new_id.readFrom(prv_key, PRV_KEY_SIZE);
    _callbacks->saveIdentity(new_id);              // <-- line 530, no result
    strcpy(reply, "OK, reboot to apply! New pubkey: ");
    mesh::Utils::toHex(&reply[33], new_id.pub_key, PUB_KEY_SIZE);
  } else {
    strcpy(reply, "Error, bad key");
  }
```

The `bool success` in that block covers the hex parse only. Nothing after the
call to `saveIdentity()` is conditional.

There is no result to check, because the interface has none.
`CommonCLI.h:221` declares:

```cpp
virtual void saveIdentity(const mesh::LocalIdentity& new_id) = 0;
```

Each of the three implementations (`examples/simple_repeater/MyMesh.cpp:1208`,
`examples/simple_room_server/MyMesh.cpp:848`,
`examples/simple_sensor/SensorMesh.cpp:800`) ends in `store.save("_main", new_id)`
and throws the answer away.

`IdentityStore::save()` does return a `bool`, and that `bool` is wrong.
`src/helpers/IdentityStore.cpp:57`:

```cpp
if (file) {
  bool success = id.writeTo(file);
  file.close();
  MESH_DEBUG_PRINTLN("IdentityStore::save() write - %s", success ? "OK" : "Err");
  return true;                    // <-- `success` goes to the debug log only
}
```

`LocalIdentity::writeTo()` (`src/Identity.cpp:117`) reports a short write
correctly. `IdentityStore::save()` receives that report, prints it in a build
with `MESH_DEBUG`, and returns `true` regardless. So the layer that knows is
the layer that does not say.

## How to reproduce

Any board. A full filesystem is the easiest way to make the write fail, and a
worn or partly erased flash region does the same thing on its own.

1. Fill the filesystem so that no new file fits. On ESP32 this is SPIFFS; write
   files until `SPIFFS.open(path, "w", true)` cannot allocate.
2. `set prv.key <64 hex chars of a valid private key>`
3. Observe: `OK, reboot to apply! New pubkey: <the key you asked for>`
4. Reboot.
5. `get pubkey` reports the OLD key, or a freshly minted one if the identity
   file was also lost.

You can see the same result without filling anything, by making
`IdentityStore::save()` take the `if (file)` false branch — it returns `false`,
and step 3 still prints `OK`.

## Why it matters

`set prv.key` is the command an operator reaches for at exactly the moment when
things have already gone wrong: a node has lost its identity and has to be put
back on the mesh under the key its peers know. The reply is the only feedback
in the loop. A reply of `OK` that means "a function returned void" sends the
operator away believing the node is restored, and the mesh finds out at the
next advert.

The failure is quiet in every direction. No serial line, no error counter, no
different reply. `MESH_DEBUG_PRINTLN` in `IdentityStore::save()` is compiled
out of every shipped build.

We hit this. An operator restoring one of four identities on an ESP32 got `OK`
from `set prv.key` and nothing was written where the firmware later looked. The
path mismatch that caused it was ours and is fixed on our side, but the command
would have reported success for a genuine flash failure in exactly the same
way, and that half is upstream's.

## The fix

Three lines of behaviour, in three files.

1. `src/helpers/IdentityStore.cpp`, in both `save()` overloads: return the
   result that the code already computed.

   ```cpp
   -    bool success = id.writeTo(file);
   -    file.close();
   -    MESH_DEBUG_PRINTLN("IdentityStore::save() write - %s", success ? "OK" : "Err");
   -    return true;
   +    bool success = id.writeTo(file);
   +    file.close();
   +    MESH_DEBUG_PRINTLN("IdentityStore::save() write - %s", success ? "OK" : "Err");
   +    return success;
   ```

   The two-argument overload returns a bare `true` in the same way and needs the
   same treatment.

2. `src/helpers/CommonCLI.h:221`: give the callback a result.

   ```cpp
   -  virtual void saveIdentity(const mesh::LocalIdentity& new_id) = 0;
   +  virtual bool saveIdentity(const mesh::LocalIdentity& new_id) = 0;
   ```

   The three in-tree implementations become `return store.save("_main", new_id);`.

3. `src/helpers/CommonCLI.cpp:530`: act on it.

   ```cpp
   -    _callbacks->saveIdentity(new_id);
   -    strcpy(reply, "OK, reboot to apply! New pubkey: ");
   -    mesh::Utils::toHex(&reply[33], new_id.pub_key, PUB_KEY_SIZE);
   +    if (!_callbacks->saveIdentity(new_id)) {
   +      strcpy(reply, "Error: identity NOT saved - key unchanged");
   +    } else {
   +      strcpy(reply, "OK, reboot to apply! New pubkey: ");
   +      mesh::Utils::toHex(&reply[33], new_id.pub_key, PUB_KEY_SIZE);
   +    }
   ```

Step 2 changes a pure virtual, so any out-of-tree class that derives from
`CommonCLICallbacks` fails to compile until its override returns a `bool`. That
is a compile error and not a silent behaviour change, and the body of each such
override already ends in a call that returns `bool`. The interface change is
the load-bearing part: the result has to travel from `IdentityStore` to the
reply, and there is no other route between them.

## Stronger still: read it back

A `bool` from `save()` reports what the filesystem API admitted to. It does not
report what is on the flash. A read-back does:

```cpp
mesh::LocalIdentity check;
if (!store.load(name, check) ||
    memcmp(check.pub_key, new_id.pub_key, PUB_KEY_SIZE) != 0) {
  // do not report success
}
```

We do this in our own equivalent command (`HydraNode::setSlotPrivateKey`,
`examples/hydra/HydraNode.cpp`), for this reason:

> Write the key. Then READ IT BACK and compare it before you report success. If
> you do not, a short or failed LittleFS write reports OK. You then find the
> fault at the next boot, and the old key is already gone.

The cost is one file open and 32 bytes of comparison, once, in a command that
already reboots the node afterwards. We suggest it, but the three-line version
above is the one that should land first: it is smaller, it is obviously
correct, and it turns a silent failure into a visible one.

## Not in scope here

The reply also says "reboot to apply" whether or not the caller can reboot, and
the command accepts a key over the air with only the admin password behind it.
Both are design questions. This report is about the one narrow claim that the
reply makes and cannot support.
