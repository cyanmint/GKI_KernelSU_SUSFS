# POSIX_MQUEUE Changes in Kernel 6.1+

## Background
In older kernels (5.10, 5.15), the `struct user_struct` had a `mq_bytes` field for tracking POSIX message queue resource limits:

```c
struct user_struct {
    ...
#ifdef CONFIG_POSIX_MQUEUE
    unsigned long mq_bytes;  /* How many bytes can be allocated to mqueue? */
#endif
    ...
};
```

This required ABI padding when enabling CONFIG_POSIX_MQUEUE=y, handled by patch `a0aa446ca326b5d26ac1dec057efd8c07d2bcbff.patch`.

## Changes in Kernel 6.1+

Starting with kernel 6.1, the POSIX_MQUEUE tracking was redesigned:

### 1. Removed from user_struct
The `mq_bytes` field was **completely removed** from `struct user_struct`.

### 2. Moved to ucounts System
Message queue accounting now uses the unified resource counting system (`struct ucounts`):

```c
struct ucounts {
    struct hlist_node node;
    struct user_namespace *ns;
    kuid_t uid;
    atomic_t count;
    atomic_long_t ucount[UCOUNT_COUNTS];
    atomic_long_t rlimit[UCOUNT_RLIMIT_COUNTS];  // <-- UCOUNT_RLIMIT_MSGQUEUE tracked here
};
```

### 3. Implementation in ipc/mqueue.c
Message queue limits are now tracked via:
- `inc_rlimit_ucounts(info->ucounts, UCOUNT_RLIMIT_MSGQUEUE, mq_bytes)`
- `dec_rlimit_ucounts(info->ucounts, UCOUNT_RLIMIT_MSGQUEUE, mq_bytes)`

Where `mq_bytes` is a **local variable**, not a struct member.

## Impact on Patches

### For Kernel 5.x (5.10, 5.15)
- ✅ **REQUIRES** patch `a0aa446ca326b5d26ac1dec057efd8c07d2bcbff.patch`
- Adds ABI padding to struct user_struct for mq_bytes field

### For Kernel 6.x (6.1, 6.6, 6.12)
- ❌ **DOES NOT NEED** patch `a0aa446ca326b5d26ac1dec057efd8c07d2bcbff.patch`
- The mq_bytes field no longer exists in user_struct
- Accounting is handled by the ucounts system which has proper ABI padding

## Conclusion

The `a0aa446ca326` patch should **ONLY** be applied to:
- android12-5.10
- android13-5.10
- android13-5.15
- android14-5.15

It should **NOT** be applied to:
- android14-6.1 ✓ (correctly omitted)
- android15-6.6 ✓ (correctly omitted)
- android16-6.12 ✓ (correctly omitted)
