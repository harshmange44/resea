#include <list.h>
#include <string.h>
#include <types.h>
#include "ipc.h"
#include "printk.h"
#include "syscall.h"
#include "task.h"

extern uint8_t __temp_page[];

/// Prefetches pages in [base, base + len) to handle page faults in advance.
static void prefetch_pages(userptr_t base, size_t len) {
    if (!len) {
        return;
    }

    userptr_t ptr = base;
    do {
        char tmp;
        // Try copying a byte from the user pointer. It may cause a page fault
        // if it's not yet mapped. If the pointer is invalid (i.e. segfault),
        // the current task will be killed in the page fault handler.
        memcpy_from_user(&tmp, ptr, sizeof(tmp));
        ptr += PAGE_SIZE;
    } while (ptr < ALIGN_UP(base + len, PAGE_SIZE));
}

/// Resumes a sender task for the `receiver` tasks and updates `receiver->src`
/// properly.
static void resume_sender(struct task *receiver, task_t src) {
    LIST_FOR_EACH (sender, &receiver->senders, struct task, sender_next) {
        if (src == IPC_ANY || src == sender->tid) {
            DEBUG_ASSERT(sender->state == TASK_BLOCKED);
            DEBUG_ASSERT(sender->src == IPC_DENY);
            task_resume(sender);
            list_remove(&sender->sender_next);

            // If src == IPC_ANY, allow only `sender` to send a message since
            // in the send phase in ipc_slowpath(), `sener` won't recheck
            // whether the `receiver` is ready for receiving from `sender`.
            receiver->src = sender->tid;
            return;
        }
    }

    receiver->src = src;
}

/// Sends and receives a message. Note that `m` is a user pointer if
/// IPC_KERNEL is not set!
static error_t ipc_slowpath(struct task *dst, task_t src, struct message *m,
                            unsigned flags) {
    // Send a message.
    if (flags & IPC_SEND) {
        // Copy the message into the receiver's buffer.
        struct message tmp_m;
        if (flags & IPC_KERNEL) {
            memcpy(&tmp_m, m, sizeof(struct message));
        } else {
            memcpy_from_user(&tmp_m, (userptr_t) m, sizeof(struct message));
        }

        if (flags & IPC_BULK) {
            if (tmp_m.bulk_len > CONFIG_BULK_BUFFER_LEN) {
                return ERR_TOO_LARGE;
            }

            // Resolve page faults in advance.
            prefetch_pages((userptr_t) tmp_m.bulk_ptr, tmp_m.bulk_len);
        }

        // Check whether the destination (receiver) task is ready for receiving.
        bool receiver_is_ready =
            dst->state == TASK_BLOCKED
            && (dst->src == IPC_ANY || dst->src == CURRENT->tid);
        if (!receiver_is_ready) {
            if (flags & IPC_NOBLOCK) {
                return ERR_WOULD_BLOCK;
            }

            // The receiver task is not ready. Sleep until it resumes the
            // current task.
            CURRENT->src = IPC_DENY;
            task_block(CURRENT);
            list_push_back(&dst->senders, &CURRENT->sender_next);
            task_switch();

            // Sanity check.
            DEBUG_ASSERT((CURRENT->notifications & NOTIFY_ABORTED)
                || (dst->src == IPC_ANY || dst->src == CURRENT->tid));

            if (CURRENT->notifications & NOTIFY_ABORTED) {
                // The receiver task has exited. Abort the system call.
                CURRENT->notifications &= ~NOTIFY_ABORTED;
                return ERR_ABORTED;
            }
        }

        // Copy the bulk payload.
        if (flags & IPC_BULK) {
            tmp_m.type |= MSG_BULK;
            tmp_m.bulk_ptr = (void *) dst->bulk_ptr;

            size_t len = tmp_m.bulk_len;
            userptr_t src_buf = (userptr_t) tmp_m.bulk_ptr;
            userptr_t dst_buf = dst->bulk_ptr;
            DEBUG_ASSERT(len <= dst->bulk_len /* it's checked by dst */);

            size_t remaining = len;
            vaddr_t temp_vaddr = (vaddr_t) __temp_page;
            while (remaining > 0) {
                offset_t offset = dst_buf % PAGE_SIZE;
                size_t copy_len = MIN(remaining, PAGE_SIZE - offset);

                // Temporarily map the page containing the receiver's buffer
                vaddr_t dst_page = ALIGN_DOWN(dst_buf, PAGE_SIZE);
                paddr_t paddr = vm_resolve(&dst->vm, dst_page);
                DEBUG_ASSERT(paddr);
                vm_link(&CURRENT->vm, temp_vaddr, paddr, PAGE_WRITABLE);

                // Copy the bulk payload into the receiver's buffer.
                memcpy_from_user(&__temp_page[offset], src_buf, copy_len);
                remaining -= copy_len;
                dst_buf += copy_len;
                src_buf += copy_len;
            }
        }

        // Copy the message.
        tmp_m.src = (flags & IPC_KERNEL) ? KERNEL_TASK_TID : CURRENT->tid;
        memcpy(&dst->m, &tmp_m, sizeof(dst->m));

        // Resume the receiver task.
        task_resume(dst);

#ifdef CONFIG_TRACE_IPC
        if (flags & IPC_BULK) {
            TRACE("IPC: %s: %s -> %s (%d bytes in bulk%s)",
                  msgtype2str(tmp_m.type), CURRENT->name, dst->name,
                  tmp_m.bulk_len, (tmp_m.type & MSG_STR) ? ", string" : "");
        } else {
            TRACE("IPC: %s: %s -> %s",
                  msgtype2str(tmp_m.type), CURRENT->name, dst->name);
        }
#endif
    }

    // Receive a message.
    if (flags & IPC_RECV) {
        struct message tmp_m;
        if (src == IPC_ANY && CURRENT->notifications) {
            // Receive pending notifications as a message.
            tmp_m.type = NOTIFICATIONS_MSG;
            tmp_m.src = KERNEL_TASK_TID;
            tmp_m.notifications.data = CURRENT->notifications;
            CURRENT->notifications = 0;
        } else {
            // Resume a sender task and sleep until a sender task resumes this
            // task...
            resume_sender(CURRENT, src);
            task_block(CURRENT);
            task_switch();

            // Copy into `tmp_m` since memcpy_to_user may cause a page fault and
            // CURRENT->m will be overwritten by page fault mesages.
            memcpy(&tmp_m, &CURRENT->m, sizeof(struct message));
        }

        // Received a message. Copy it into the receiver buffer.
        if (flags & IPC_KERNEL) {
            memcpy(m, &tmp_m, sizeof(struct message));
        } else {
            memcpy_to_user((userptr_t) m, &tmp_m, sizeof(struct message));
        }
    }

    return OK;
}

/// The IPC fastpath: an IPC implementation optimized for the common case.
///
/// Note that `m` is a user pointer if IPC_KERNEL is not set!
error_t ipc(struct task *dst, task_t src, struct message *m, unsigned flags) {
    // Resolve page faults in advance.
    if ((flags & IPC_RECV) && !(flags & IPC_KERNEL) && CURRENT->bulk_ptr) {
        prefetch_pages(CURRENT->bulk_ptr, CURRENT->bulk_len);
    }

#ifdef CONFIG_IPC_FASTPATH
    int fastpath =
        (flags & ~IPC_NOBLOCK) == IPC_CALL
        && dst
        && dst->state == TASK_BLOCKED
        && (dst->src == IPC_ANY || dst->src == CURRENT->tid)
        && CURRENT->notifications == 0;

    if (!fastpath) {
        return ipc_slowpath(dst, src, m, flags);
    }

    // THe send phase: copy the message and resume the receiver task. Note
    // that this user copy may cause a page fault.
    memcpy_from_user(&dst->m, (userptr_t) m, sizeof(struct message));
    DEBUG_ASSERT((dst->m.type & MSG_BULK) == 0); // FIXME:

#ifdef CONFIG_TRACE_IPC
    TRACE("IPC: %s: %s -> %s (fastpath)",
          msgtype2str(dst->m.type), CURRENT->name, dst->name);
#endif

    // We just ignore invalid flags. To eliminate branches for performance,
    // we don't return an error code.
    dst->m.type = MSG_ID(dst->m.type);
    dst->m.src = CURRENT->tid;

    task_resume(dst);

    // The receive phase: wait for a message, copy it into the user's
    // buffer, and return to the user.
    resume_sender(CURRENT, src);
    task_block(CURRENT);

    task_switch();

    // This user copy should not cause a page fault since we've filled the
    // page in the user copy above.
    memcpy_to_user((userptr_t) m, &CURRENT->m, sizeof(struct message));
    return OK;
#else
    return ipc_slowpath(dst, src, m, flags);
#endif // CONFIG_IPC_FASTPATH
}

// Notifies notifications to the task.
void notify(struct task *dst, notifications_t notifications) {
    if (dst->state == TASK_BLOCKED && dst->src == IPC_ANY) {
        // Send a NOTIFICATIONS_MSG message immediately.
        dst->m.type = NOTIFICATIONS_MSG;
        dst->m.src = KERNEL_TASK_TID;
        dst->m.notifications.data = dst->notifications | notifications;
        dst->notifications = 0;
        task_resume(dst);
    } else {
        // The task is not ready for receiving a event message: update the
        // pending notifications instead.
        dst->notifications |= notifications;
    }
}
