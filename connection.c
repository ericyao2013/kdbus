/*
 * Copyright (C) 2013-2014 Kay Sievers
 * Copyright (C) 2013-2014 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013-2014 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 * Copyright (C) 2013-2014 Linux Foundation
 * Copyright (C) 2014 Djalal Harouni
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/audit.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/shmem_fs.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/syscalls.h>

#include "bus.h"
#include "connection.h"
#include "endpoint.h"
#include "match.h"
#include "message.h"
#include "metadata.h"
#include "names.h"
#include "domain.h"
#include "item.h"
#include "notify.h"
#include "policy.h"
#include "util.h"
#include "queue.h"

struct kdbus_conn_reply;

#define KDBUS_CONN_ACTIVE_BIAS (INT_MIN + 1)

/**
 * struct kdbus_conn_reply - an entry of kdbus_conn's list of replies
 * @entry:		The entry of the connection's reply_list
 * @conn:		The counterpart connection that is expected to answer
 * @queue_entry:	The queue enty item that is prepared by the replying
 *			connection
 * @deadline_ns:	The deadline of the reply, in nanoseconds
 * @cookie:		The cookie of the requesting message
 * @sync:		The reply block is waiting for synchronous I/O
 * @waiting:		The condition to synchronously wait for
 * @err:		The error code for the synchronous reply
 */
struct kdbus_conn_reply {
	struct list_head entry;
	struct kdbus_conn *conn;
	struct kdbus_queue_entry *queue_entry;
	u64 deadline_ns;
	u64 cookie;
	bool sync:1;
	bool waiting:1;
	int err;
};

static void kdbus_conn_reply_free(struct kdbus_conn_reply *reply)
{
	atomic_dec(&reply->conn->reply_count);
	kdbus_conn_unref(reply->conn);
	kfree(reply);
}

static void kdbus_conn_reply_sync(struct kdbus_conn_reply *reply, int err)
{
	BUG_ON(!reply->sync);

	list_del_init(&reply->entry);
	reply->waiting = false;
	reply->err = err;
	wake_up_interruptible(&reply->conn->wait);
}

/*
 * Check for maximum number of messages per individual user. This
 * should prevent a single user from being able to fill the receiver's
 * queue.
 */
static int kdbus_conn_queue_user_quota(struct kdbus_conn *conn,
				       const struct kdbus_conn *conn_src,
				       struct kdbus_queue_entry *entry)
{
	unsigned int user;

	if (!conn_src)
		return 0;

	if (ns_capable(&init_user_ns, CAP_IPC_OWNER))
		return 0;

	/*
	 * Only after the queue grows above the maximum number of messages
	 * per individual user, we start to count all further messages
	 * from the sending users.
	 */
	if (conn->queue.msg_count < KDBUS_CONN_MAX_MSGS_PER_USER)
		return 0;

	user = conn_src->user->idr;

	/* extend array to store the user message counters */
	if (user >= conn->msg_users_max) {
		unsigned int *users;
		unsigned int i;

		i = 8 + KDBUS_ALIGN8(user);
		users = kcalloc(i, sizeof(unsigned int), GFP_KERNEL);
		if (!users)
			return -ENOMEM;

		memcpy(users, conn->msg_users,
		       sizeof(unsigned int) * conn->msg_users_max);
		kfree(conn->msg_users);
		conn->msg_users = users;
		conn->msg_users_max = i;
	}

	if (conn->msg_users[user] > KDBUS_CONN_MAX_MSGS_PER_USER)
		return -ENOBUFS;

	conn->msg_users[user]++;
	entry->user = user;
	return 0;
}

static void kdbus_conn_work(struct work_struct *work)
{
	struct kdbus_conn *conn;
	struct kdbus_conn_reply *reply, *reply_tmp;
	LIST_HEAD(reply_list);
	u64 deadline = ~0ULL;
	struct timespec64 ts;
	u64 now;

	conn = container_of(work, struct kdbus_conn, work.work);
	ktime_get_ts64(&ts);
	now = timespec64_to_ns(&ts);

	mutex_lock(&conn->lock);
	if (!kdbus_conn_active(conn)) {
		mutex_unlock(&conn->lock);
		return;
	}

	list_for_each_entry_safe(reply, reply_tmp, &conn->reply_list, entry) {
		/*
		 * If the reply block is waiting for synchronous I/O,
		 * the timeout is handled by wait_event_*_timeout(),
		 * so we don't have to care for it here.
		 */
		if (reply->sync)
			continue;

		if (reply->deadline_ns > now) {
			/* remember next timeout */
			if (deadline > reply->deadline_ns)
				deadline = reply->deadline_ns;

			continue;
		}

		/*
		 * Move to temporary cleanup list; we cannot unref and
		 * possibly cleanup a connection that is holding a ref
		 * back to us, while we are locking ourselves.
		 */
		list_move_tail(&reply->entry, &reply_list);

		/*
		 * A zero deadline means the connection died, was
		 * cleaned up already and the notify sent.
		 */
		if (reply->deadline_ns == 0)
			continue;

		kdbus_notify_reply_timeout(conn->bus, reply->conn->id,
					   reply->cookie);
	}

	/* rearm delayed work with next timeout */
	if (deadline != ~0ULL)
		schedule_delayed_work(&conn->work,
				      nsecs_to_jiffies(deadline - now));

	mutex_unlock(&conn->lock);

	kdbus_notify_flush(conn->bus);

	list_for_each_entry_safe(reply, reply_tmp, &reply_list, entry)
		kdbus_conn_reply_free(reply);
}

/**
 * kdbus_cmd_msg_recv() - receive a message from the queue
 * @conn:		Connection to work on
 * @recv:		The command as passed in by the ioctl
 *
 * Return: 0 on success, negative errno on failure
 */
int kdbus_cmd_msg_recv(struct kdbus_conn *conn,
		       struct kdbus_cmd_recv *recv)
{
	struct kdbus_queue_entry *entry = NULL;
	int ret;

	/* Reject unknown flags */
	if (recv->flags & ~(KDBUS_RECV_PEEK |
			    KDBUS_RECV_DROP |
			    KDBUS_RECV_USE_PRIORITY))
		return -EOPNOTSUPP;

	if (recv->offset > 0)
		return -EINVAL;

	mutex_lock(&conn->lock);
	ret = kdbus_queue_entry_peek(&conn->queue,
				     recv->priority,
				     recv->flags & KDBUS_RECV_USE_PRIORITY,
				     &entry);
	if (ret < 0)
		goto exit_unlock;

	BUG_ON(!entry);

	/* just drop the message */
	if (recv->flags & KDBUS_RECV_DROP) {
		struct kdbus_conn_reply *reply = NULL;
		bool reply_found = false;

		if (entry->reply) {
			struct kdbus_conn_reply *r;

			/*
			 * Walk the list of pending replies and see if the
			 * one attached to this entry item is stil there.
			 * It might have been removed by an incoming reply,
			 * and we currently don't track reply entries in that
			 * direction in order to prevent potentially dangling
			 * pointers.
			 */
			list_for_each_entry(r, &conn->reply_list, entry) {
				if (r == entry->reply) {
					reply_found = true;
					break;
				}
			}
		}

		if (reply_found) {
			if (entry->reply->sync) {
				kdbus_conn_reply_sync(entry->reply, -EPIPE);
			} else {
				list_del_init(&entry->reply->entry);
				reply = entry->reply;
			}

			kdbus_notify_reply_dead(conn->bus,
						entry->src_id,
						entry->cookie);
		}

		kdbus_queue_entry_remove(conn, entry);
		kdbus_pool_slice_free(entry->slice);
		mutex_unlock(&conn->lock);

		if (reply)
			kdbus_conn_reply_free(reply);

		kdbus_queue_entry_free(entry);

		goto exit;
	}

	/* Give the offset back to the caller. */
	recv->offset = kdbus_pool_slice_offset(entry->slice);

	/*
	 * Just return the location of the next message. Do not install
	 * file descriptors or anything else. This is usually used to
	 * determine the sender of the next queued message.
	 *
	 * File descriptor numbers referenced in the message items
	 * are undefined, they are only valid with the full receive
	 * not with peek.
	 */
	if (recv->flags & KDBUS_RECV_PEEK) {
		kdbus_pool_slice_flush(entry->slice);
		goto exit_unlock;
	}

	ret = kdbus_queue_entry_install(entry);
	kdbus_pool_slice_make_public(entry->slice);
	kdbus_queue_entry_remove(conn, entry);
	kdbus_queue_entry_free(entry);

exit_unlock:
	mutex_unlock(&conn->lock);
exit:
	kdbus_notify_flush(conn->bus);
	return ret;
}

/**
 * kdbus_cmd_msg_cancel() - cancel all pending sync requests
 *			    with the given cookie
 * @conn:		The connection
 * @cookie:		The cookie
 *
 * Return: 0 on success, or -ENOENT if no pending request with that
 * cookie was found.
 */
int kdbus_cmd_msg_cancel(struct kdbus_conn *conn,
			 u64 cookie)
{
	struct kdbus_conn_reply *reply, *reply_tmp;
	struct kdbus_conn *c;
	bool found = false;
	int i;

	if (atomic_read(&conn->reply_count) == 0)
		return -ENOENT;

	/* lock order: domain -> bus -> ep -> names -> conn */
	down_read(&conn->bus->conn_rwlock);
	hash_for_each(conn->bus->conn_hash, i, c, hentry) {
		if (c == conn)
			continue;

		mutex_lock(&c->lock);
		list_for_each_entry_safe(reply, reply_tmp,
					 &c->reply_list, entry) {
			if (reply->sync &&
			    reply->conn == conn &&
			    reply->cookie == cookie) {
				kdbus_conn_reply_sync(reply, -ECANCELED);
				found = true;
			}
		}
		mutex_unlock(&c->lock);
	}
	up_read(&conn->bus->conn_rwlock);

	return found ? 0 : -ENOENT;
}

static int kdbus_conn_check_access(struct kdbus_ep *ep,
				   const struct kdbus_msg *msg,
				   struct kdbus_conn *conn_src,
				   struct kdbus_conn *conn_dst,
				   struct kdbus_conn_reply **reply_wake)
{
	bool allowed = false;
	int ret;

	/*
	 * Walk the conn_src's list of expected replies.
	 * If there's any matching entry, allow the message to
	 * be sent, and remove the entry.
	 */
	if (reply_wake && msg->cookie_reply > 0) {
		struct kdbus_conn_reply *r, *r_tmp;
		LIST_HEAD(reply_list);

		mutex_lock(&conn_src->lock);
		list_for_each_entry_safe(r, r_tmp,
					 &conn_src->reply_list,
					 entry) {
			if (r->conn == conn_dst &&
			    r->cookie == msg->cookie_reply) {
				if (r->sync)
					*reply_wake = r;
				else
					list_move_tail(&r->entry, &reply_list);

				allowed = true;
				break;
			}
		}
		mutex_unlock(&conn_src->lock);

		list_for_each_entry_safe(r, r_tmp, &reply_list, entry)
			kdbus_conn_reply_free(r);
	}

	if (allowed)
		return 0;

	/* ... otherwise, ask the policy DBs for permission */
	ret = kdbus_ep_policy_check_talk_access(ep, conn_src, conn_dst);
	if (ret < 0)
		return ret;

	return 0;
}

static int kdbus_conn_add_expected_reply(struct kdbus_conn *conn_src,
					 struct kdbus_conn *conn_dst,
					 const struct kdbus_msg *msg,
					 struct kdbus_conn_reply **reply_wait)
{
	bool sync = msg->flags & KDBUS_MSG_FLAGS_SYNC_REPLY;
	struct kdbus_conn_reply *r;
	struct timespec64 ts;
	int ret = 0;

	if (atomic_inc_return(&conn_src->reply_count) >
	    KDBUS_CONN_MAX_REQUESTS_PENDING) {
		ret = -EMLINK;
		goto exit_dec_reply_count;
	}

	mutex_lock(&conn_dst->lock);
	if (!kdbus_conn_active(conn_dst)) {
		ret = -ECONNRESET;
		goto exit_unlock;
	}

	/*
	 * This message expects a reply, so let's interpret
	 * msg->timeout_ns and add a kdbus_conn_reply object.
	 * Add it to the list of expected replies on the
	 * destination connection.
	 * When a reply is received later on, this entry will
	 * be used to allow the reply to pass, circumventing the
	 * policy.
	 */
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		ret = -ENOMEM;
		goto exit_unlock;
	}

	r->conn = kdbus_conn_ref(conn_src);
	r->cookie = msg->cookie;

	if (sync) {
		r->sync = true;
		r->waiting = true;
	} else {
		/* calculate the deadline based on the current time */
		ktime_get_ts64(&ts);
		r->deadline_ns = timespec64_to_ns(&ts) + msg->timeout_ns;
	}

	list_add(&r->entry, &conn_dst->reply_list);
	*reply_wait = r;

	/*
	 * For async operation, schedule the scan now. It won't do
	 * any real work at this point, but walk the list of all
	 * pending replies and rearm the connection's delayed work
	 * to the closest entry.
	 * For synchronous operation, the timeout will be handled
	 * by wait_event_interruptible_timeout().
	 */
	if (!sync)
		schedule_delayed_work(&conn_dst->work, 0);

exit_unlock:
	mutex_unlock(&conn_dst->lock);

exit_dec_reply_count:
	if (ret < 0)
		atomic_dec(&conn_src->reply_count);

	return ret;
}

/* enqueue a message into the receiver's pool */
static int kdbus_conn_entry_insert(struct kdbus_conn *conn,
				   struct kdbus_conn *conn_src,
				   const struct kdbus_kmsg *kmsg,
				   struct kdbus_conn_reply *reply)
{
	struct kdbus_queue_entry *entry;
	int ret;

	mutex_lock(&conn->lock);

	/* limit the maximum number of queued messages */
	if (!ns_capable(&init_user_ns, CAP_IPC_OWNER) &&
	    conn->queue.msg_count > KDBUS_CONN_MAX_MSGS) {
		ret = -ENOBUFS;
		goto exit_unlock;
	}

	if (!kdbus_conn_active(conn)) {
		ret = -ECONNRESET;
		goto exit_unlock;
	}

	if (kmsg->fds && !(conn->flags & KDBUS_HELLO_ACCEPT_FD)) {
		ret = -ECOMM;
		goto exit_unlock;
	}

	ret = kdbus_queue_entry_alloc(conn, kmsg, &entry);
	if (ret < 0)
		goto exit_unlock;

	/* limit the number of queued messages from the same individual user */
	ret = kdbus_conn_queue_user_quota(conn, conn_src, entry);
	if (ret < 0)
		goto exit_queue_free;

	/*
	 * Remember the the reply associated with this queue entry, so we can
	 * move the reply entry's connection when a connection moves from an
	 * activator to an implementor.
	 */
	entry->reply = reply;

	/* link the message into the receiver's entry */
	kdbus_queue_entry_add(&conn->queue, entry);
	mutex_unlock(&conn->lock);

	/* wake up poll() */
	wake_up_interruptible(&conn->wait);
	return 0;

exit_queue_free:
	kdbus_queue_entry_free(entry);
exit_unlock:
	mutex_unlock(&conn->lock);
	return ret;
}

static int kdbus_kmsg_attach_metadata(struct kdbus_kmsg *kmsg,
				      struct kdbus_conn *conn_src,
				      struct kdbus_conn *conn_dst)
{
	u64 attach_flags;

	/*
	 * Append metadata items according to the destination connection's
	 * attach flags. If the source connection has faked credentials, the
	 * metadata object associated with the kmsg has been pre-filled with
	 * conn_src->owner_meta, and we only attach the connection's name and
	 * currently owned names on top of that.
	 */
	mutex_lock(&conn_dst->lock);
	attach_flags = conn_dst->attach_flags;
	mutex_unlock(&conn_dst->lock);

	if (conn_src->owner_meta)
		attach_flags &= KDBUS_ATTACH_NAMES | KDBUS_ATTACH_CONN_NAME;

	return kdbus_meta_append(kmsg->meta, conn_src, kmsg->seq, attach_flags);
}

static void kdbus_conn_broadcast(struct kdbus_ep *ep,
				 struct kdbus_conn *conn_src,
				 struct kdbus_kmsg *kmsg)
{
	const struct kdbus_msg *msg = &kmsg->msg;
	struct kdbus_bus *bus = ep->bus;
	struct kdbus_conn *conn_dst;
	unsigned int i;
	int ret = 0;

	down_read(&bus->conn_rwlock);

	hash_for_each(bus->conn_hash, i, conn_dst, hentry) {
		if (conn_dst->id == msg->src_id)
			continue;

		/*
		 * Activator or policy holder connections will
		 * not receive any broadcast messages, only
		 * ordinary and monitor ones.
		 */
		if (!kdbus_conn_is_connected(conn_dst) &&
		    !kdbus_conn_is_monitor(conn_dst))
			continue;

		if (!kdbus_match_db_match_kmsg(conn_dst->match_db, conn_src,
					       kmsg))
			continue;

		ret = kdbus_ep_policy_check_notification(conn_dst->ep,
							 conn_dst, kmsg);
		if (ret < 0)
			continue;

		/*
		 * The first receiver which requests additional
		 * metadata causes the message to carry it; all
		 * receivers after that will see all of the added
		 * data, even when they did not ask for it.
		 */
		if (conn_src) {
			/* Check if conn_src is allowed to signal */
			ret = kdbus_ep_policy_check_broadcast(conn_dst->ep,
							      conn_src,
							      conn_dst);
			if (ret < 0)
				continue;

			ret = kdbus_ep_policy_check_src_names(conn_dst->ep,
							      conn_src,
							      conn_dst);
			if (ret < 0)
				continue;

			ret = kdbus_kmsg_attach_metadata(kmsg, conn_src,
							 conn_dst);
			if (ret < 0)
				goto exit_unlock;
		}

		kdbus_conn_entry_insert(conn_dst, conn_src, kmsg, NULL);
	}

exit_unlock:
	up_read(&bus->conn_rwlock);
}

static void kdbus_conn_eavesdrop(struct kdbus_ep *ep, struct kdbus_conn *conn,
				 struct kdbus_kmsg *kmsg)
{
	struct kdbus_conn *c;
	int ret;

	/*
	 * Monitor connections get all messages; ignore possible errors
	 * when sending messages to monitor connections.
	 */

	down_read(&ep->bus->conn_rwlock);
	list_for_each_entry(c, &ep->bus->monitors_list, monitor_entry) {
		/*
		 * The first monitor which requests additional
		 * metadata causes the message to carry it; all
		 * monitors after that will see all of the added
		 * data, even when they did not ask for it.
		 */
		if (conn) {
			ret = kdbus_kmsg_attach_metadata(kmsg, conn, c);
			if (ret < 0)
				break;
		}

		kdbus_conn_entry_insert(c, NULL, kmsg, NULL);
	}
	up_read(&ep->bus->conn_rwlock);
}

/**
 * kdbus_conn_kmsg_send() - send a message
 * @ep:			Endpoint to send from
 * @conn_src:		Connection, kernel-generated messages do not have one
 * @kmsg:		Message to send
 *
 * Return: 0 on success, negative errno on failure
 */
int kdbus_conn_kmsg_send(struct kdbus_ep *ep,
			 struct kdbus_conn *conn_src,
			 struct kdbus_kmsg *kmsg)
{
	struct kdbus_conn_reply *reply_wait = NULL;
	struct kdbus_conn_reply *reply_wake = NULL;
	struct kdbus_name_entry *name_entry = NULL;
	const struct kdbus_msg *msg = &kmsg->msg;
	struct kdbus_conn *conn_dst = NULL;
	struct kdbus_bus *bus = ep->bus;
	bool sync = msg->flags & KDBUS_MSG_FLAGS_SYNC_REPLY;
	int ret = 0;

	/* assign domain-global message sequence number */
	BUG_ON(kmsg->seq > 0);
	kmsg->seq = atomic64_inc_return(&bus->domain->msg_seq_last);

	/* non-kernel senders append credentials/metadata */
	if (conn_src) {
		/*
		 * If a connection has installed faked credentials when it was
		 * created, make sure only those are sent out as attachments
		 * of messages, and nothing that is gathered at retrieved from
		 * 'current' at the time of sending.
		 *
		 * Hence, in such cases, duplicate the connection's owner_meta,
		 * and take care not to augment it by attaching any new items.
		 */
		if (conn_src->owner_meta)
			ret = kdbus_meta_dup(conn_src->owner_meta, &kmsg->meta);
		else
			ret = kdbus_meta_new(&kmsg->meta);

		if (ret < 0)
			return ret;
	}

	if (msg->dst_id == KDBUS_DST_ID_BROADCAST) {
		kdbus_conn_broadcast(ep, conn_src, kmsg);
		return 0;
	}

	if (kmsg->dst_name) {
		name_entry = kdbus_name_lock(bus->name_registry,
					     kmsg->dst_name);
		if (!name_entry)
			return -ESRCH;

		/*
		 * If both a name and a connection ID are given as destination
		 * of a message, check that the currently owning connection of
		 * the name matches the specified ID.
		 * This way, we allow userspace to send the message to a
		 * specific connection by ID only if the connection currently
		 * owns the given name.
		 */
		if (msg->dst_id != KDBUS_DST_ID_NAME &&
		    msg->dst_id != name_entry->conn->id) {
			ret = -EREMCHG;
			goto exit_name_unlock;
		}

		if (!name_entry->conn && name_entry->activator)
			conn_dst = kdbus_conn_ref(name_entry->activator);
		else
			conn_dst = kdbus_conn_ref(name_entry->conn);

		if ((msg->flags & KDBUS_MSG_FLAGS_NO_AUTO_START) &&
		     kdbus_conn_is_activator(conn_dst)) {
			ret = -EADDRNOTAVAIL;
			goto exit_unref;
		}
	} else {
		/* unicast message to unique name */
		conn_dst = kdbus_bus_find_conn_by_id(bus, msg->dst_id);
		if (!conn_dst)
			return -ENXIO;

		/*
		 * Special-purpose connections are not allowed to be addressed
		 * via their unique IDs.
		 */
		if (!kdbus_conn_is_connected(conn_dst)) {
			ret = -ENXIO;
			goto exit_unref;
		}
	}

	/*
	 * Record the sequence number of the registered name;
	 * it will be passed on to the queue, in case messages
	 * addressed to a name need to be moved from or to
	 * activator connections of the same name.
	 */
	if (name_entry)
		kmsg->dst_name_id = name_entry->name_id;

	if (conn_src) {
		if (msg->flags & KDBUS_MSG_FLAGS_EXPECT_REPLY) {
			ret = kdbus_conn_check_access(ep, msg, conn_src,
						      conn_dst, NULL);
			if (ret < 0)
				goto exit_unref;

			ret = kdbus_conn_add_expected_reply(conn_src, conn_dst,
							    msg, &reply_wait);
			if (ret < 0)
				goto exit_unref;
		} else {
			ret = kdbus_conn_check_access(ep, msg, conn_src,
						      conn_dst, &reply_wake);
			if (ret < 0)
				goto exit_unref;
		}

		ret = kdbus_kmsg_attach_metadata(kmsg, conn_src, conn_dst);
		if (ret < 0)
			goto exit_unref;
	}

	if (reply_wake) {
		/*
		 * If we're synchronously responding to a message, allocate a
		 * queue item and attach it to the reply tracking object.
		 * The connection's queue will never get to see it.
		 */
		mutex_lock(&conn_dst->lock);
		if (kdbus_conn_active(conn_dst))
			ret = kdbus_queue_entry_alloc(conn_dst, kmsg,
						      &reply_wake->queue_entry);
		else
			ret = -ECONNRESET;

		kdbus_conn_reply_sync(reply_wake, ret);
		mutex_unlock(&conn_dst->lock);
	} else {
		/*
		 * Otherwise, put it in the queue and wait for the connection
		 * to dequeue and receive the message.
		 */
		ret = kdbus_conn_entry_insert(conn_dst, conn_src,
					      kmsg, reply_wait);
	}

	if (ret < 0)
		goto exit_unref;

	/* forward to monitors */
	kdbus_conn_eavesdrop(ep, conn_src, kmsg);

	/* no reason to keep names locked for replies */
	name_entry = kdbus_name_unlock(bus->name_registry, name_entry);

	if (sync) {
		int r;
		struct kdbus_queue_entry *entry;
		u64 usecs = div_u64(msg->timeout_ns, 1000ULL);

		BUG_ON(!reply_wait);

		/*
		 * Block until the reply arrives. reply_wait is left untouched
		 * by the timeout scans that might be conducted for other,
		 * asynchronous replies of conn_src.
		 */
		r = wait_event_interruptible_timeout(reply_wait->conn->wait,
			!reply_wait->waiting || !kdbus_conn_active(conn_src),
			usecs_to_jiffies(usecs));
		if (r == 0)
			ret = -ETIMEDOUT;
		else if (r < 0)
			ret = -EINTR;
		else if (!kdbus_conn_active(conn_src))
			ret = -ECONNRESET;
		else
			ret = reply_wait->err;

		mutex_lock(&conn_dst->lock);
		list_del_init(&reply_wait->entry);
		mutex_unlock(&conn_dst->lock);

		mutex_lock(&conn_src->lock);
		entry = reply_wait->queue_entry;
		if (entry) {
			if (ret == 0)
				ret = kdbus_queue_entry_install(entry);

			kmsg->msg.offset_reply =
				kdbus_pool_slice_offset(entry->slice);
			kdbus_pool_slice_make_public(entry->slice);
			kdbus_queue_entry_free(entry);
		}
		mutex_unlock(&conn_src->lock);

		kdbus_conn_reply_free(reply_wait);
	}

exit_unref:
	kdbus_conn_unref(conn_dst);
exit_name_unlock:
	kdbus_name_unlock(bus->name_registry, name_entry);

	return ret;
}

/**
 * kdbus_conn_disconnect() - disconnect a connection
 * @conn:		The connection to disconnect
 * @ensure_queue_empty:	Flag to indicate if the call should fail in
 *			case the connection's message list is not
 *			empty
 *
 * If @ensure_msg_list_empty is true, and the connection has pending messages,
 * -EBUSY is returned.
 *
 * Return: 0 on success, negative errno on failure
 */
int kdbus_conn_disconnect(struct kdbus_conn *conn, bool ensure_queue_empty)
{
	struct kdbus_conn_reply *reply, *reply_tmp;
	struct kdbus_queue_entry *entry, *tmp;
	LIST_HEAD(reply_list);

	mutex_lock(&conn->lock);
	if (!kdbus_conn_active(conn)) {
		mutex_unlock(&conn->lock);
		return -EALREADY;
	}

	if (ensure_queue_empty && !list_empty(&conn->queue.msg_list)) {
		mutex_unlock(&conn->lock);
		return -EBUSY;
	}

	atomic_add(KDBUS_CONN_ACTIVE_BIAS, &conn->active);
	mutex_unlock(&conn->lock);

	wake_up_interruptible(&conn->wait);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	rwsem_acquire(&conn->dep_map, 0, 0, _RET_IP_);
	if (atomic_read(&conn->active) != KDBUS_CONN_ACTIVE_BIAS)
		lock_contended(&conn->dep_map, _RET_IP_);
#endif

	wait_event(conn->wait,
		   atomic_read(&conn->active) == KDBUS_CONN_ACTIVE_BIAS);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	lock_acquired(&conn->dep_map, _RET_IP_);
	rwsem_release(&conn->dep_map, 1, _RET_IP_);
#endif

	cancel_delayed_work_sync(&conn->work);

	/* lock order: domain -> bus -> ep -> names -> conn */
	down_write(&conn->bus->conn_rwlock);
	mutex_lock(&conn->ep->lock);

	/* remove from bus and endpoint */
	hash_del(&conn->hentry);
	list_del(&conn->monitor_entry);
	list_del(&conn->ep_entry);

	mutex_unlock(&conn->ep->lock);
	up_write(&conn->bus->conn_rwlock);

	/*
	 * Remove all names associated with this connection; this possibly
	 * moves queued messages back to the activator connection.
	 */
	kdbus_name_remove_by_conn(conn->bus->name_registry, conn);

	/* if we die while other connections wait for our reply, notify them */
	mutex_lock(&conn->lock);
	list_for_each_entry_safe(entry, tmp, &conn->queue.msg_list, entry) {
		if (entry->reply)
			kdbus_notify_reply_dead(conn->bus, entry->src_id,
						entry->cookie);

		kdbus_queue_entry_remove(conn, entry);
		kdbus_pool_slice_free(entry->slice);
		kdbus_queue_entry_free(entry);
	}
	list_splice_init(&conn->reply_list, &reply_list);
	mutex_unlock(&conn->lock);

	list_for_each_entry_safe(reply, reply_tmp, &reply_list, entry) {
		if (reply->sync) {
			kdbus_conn_reply_sync(reply, -EPIPE);
			continue;
		}

		/* send a 'connection dead' notification */
		kdbus_notify_reply_dead(conn->bus, reply->conn->id,
					reply->cookie);

		list_del(&reply->entry);
		kdbus_conn_reply_free(reply);
	}

	kdbus_notify_id_change(conn->bus, KDBUS_ITEM_ID_REMOVE,
			       conn->id, conn->flags);

	kdbus_notify_flush(conn->bus);

	return 0;
}

/**
 * kdbus_conn_active() - connection is not disconnected
 * @conn:		Connection to check
 *
 * Return true if the connection was not disconnected, yet. Note that a
 * connection might be disconnected asynchronously, unless you hold the
 * connection lock. If that's not suitable for you, see kdbus_conn_acquire() to
 * suppress connection shutdown for a short period.
 *
 * Return: true if the connection is still active
 */
bool kdbus_conn_active(const struct kdbus_conn *conn)
{
	return atomic_read(&conn->active) >= 0;
}

/**
 * kdbus_conn_flush_policy() - flush all cached policy entries that
 *			       refer to a connecion
 * @conn:	Connection to check
 */
void kdbus_conn_purge_policy_cache(struct kdbus_conn *conn)
{
	kdbus_policy_purge_cache(&conn->ep->policy_db, conn);
	kdbus_policy_purge_cache(&conn->bus->policy_db, conn);
}

static void __kdbus_conn_free(struct kref *kref)
{
	struct kdbus_conn *conn = container_of(kref, struct kdbus_conn, kref);

	BUG_ON(kdbus_conn_active(conn));
	BUG_ON(delayed_work_pending(&conn->work));
	BUG_ON(!list_empty(&conn->queue.msg_list));
	BUG_ON(!list_empty(&conn->names_list));
	BUG_ON(!list_empty(&conn->names_queue_list));
	BUG_ON(!list_empty(&conn->reply_list));

	atomic_dec(&conn->user->connections);
	kdbus_domain_user_unref(conn->user);

	kdbus_conn_purge_policy_cache(conn);
	kdbus_policy_remove_owner(&conn->bus->policy_db, conn);

	kdbus_meta_free(conn->owner_meta);
	kdbus_match_db_free(conn->match_db);
	kdbus_pool_free(conn->pool);
	kdbus_ep_unref(conn->ep);
	kdbus_bus_unref(conn->bus);
	put_cred(conn->cred);
	kfree(conn->name);
	kfree(conn);
}

/**
 * kdbus_conn_ref() - take a connection reference
 * @conn:		Connection
 *
 * Return: the connection itself
 */
struct kdbus_conn *kdbus_conn_ref(struct kdbus_conn *conn)
{
	kref_get(&conn->kref);
	return conn;
}

/**
 * kdbus_conn_unref() - drop a connection reference
 * @conn:		Connection (may be NULL)
 *
 * When the last reference is dropped, the connection's internal structure
 * is freed.
 *
 * Return: NULL
 */
struct kdbus_conn *kdbus_conn_unref(struct kdbus_conn *conn)
{
	if (!conn)
		return NULL;

	kref_put(&conn->kref, __kdbus_conn_free);
	return NULL;
}

/**
 * kdbus_conn_acquire() - acquire an active connection reference
 * @conn:		Connection
 *
 * Users can close a connection via KDBUS_BYEBYE (or by destroying the
 * endpoint/bus/...) at any time. Whenever this happens, we should deny any
 * user-visible action on this connection and signal ECONNRESET instead.
 * To avoid testing for connection availability everytime you take the
 * connection-lock, you can acquire a connection for short periods.
 *
 * By calling kdbus_conn_acquire(), you gain an "active reference" to the
 * connection. You must also hold a regular reference at any time! As long as
 * you hold the active-ref, the connection will not be shut down. However, if
 * the connection was shut down, you can never acquire an active-ref again.
 *
 * kdbus_conn_disconnect() disables the connection and then waits for all active
 * references to be dropped. It will also wake up any pending operation.
 * However, you must not sleep for an indefinite period while holding an
 * active-reference. Otherwise, kdbus_conn_disconnect() might stall. If you need
 * to sleep for an indefinite period, either release the reference and try to
 * acquire it again after waking up, or make kdbus_conn_disconnect() wake up
 * your wait-queue.
 *
 * Return: 0 on success, negative error code on failure.
 */
int kdbus_conn_acquire(struct kdbus_conn *conn)
{
	if (!atomic_inc_unless_negative(&conn->active))
		return -ECONNRESET;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	rwsem_acquire_read(&conn->dep_map, 0, 1, _RET_IP_);
#endif

	return 0;
}

/**
 * kdbus_conn_release() - release an active connection reference
 * @conn:		Connection
 *
 * This releases an active reference that has been acquired via
 * kdbus_conn_acquire(). If the connection was already disabled and this is the
 * last active-ref that is dropped, the disconnect-waiter will be woken up and
 * properly close the connection.
 */
void kdbus_conn_release(struct kdbus_conn *conn)
{
	int v;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	rwsem_release(&conn->dep_map, 1, _RET_IP_);
#endif

	v = atomic_dec_return(&conn->active);
	if (v != KDBUS_CONN_ACTIVE_BIAS)
		return;

	wake_up_all(&conn->wait);
}

/**
 * kdbus_conn_move_messages() - move messages from one connection to another
 * @conn_dst:		Connection to copy to
 * @conn_src:		Connection to copy from
 * @name_id:		Filter for the sequence number of the registered
 *			name, 0 means no filtering.
 *
 * Move all messages from one connection to another. This is used when
 * an implementor connection is taking over/giving back a well-known name
 * from/to an activator connection.
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_conn_move_messages(struct kdbus_conn *conn_dst,
			     struct kdbus_conn *conn_src,
			     u64 name_id)
{
	struct kdbus_queue_entry *q, *q_tmp;
	LIST_HEAD(reply_list);
	LIST_HEAD(msg_list);
	int ret = 0;

	BUG_ON(!mutex_is_locked(&conn_dst->bus->lock));
	BUG_ON(conn_src == conn_dst);

	/* remove all messages from the source */
	mutex_lock(&conn_src->lock);
	list_splice_init(&conn_src->reply_list, &reply_list);
	list_for_each_entry_safe(q, q_tmp, &conn_src->queue.msg_list, entry) {
		kdbus_queue_entry_remove(conn_src, q);
		list_add_tail(&q->entry, &msg_list);
	}
	mutex_unlock(&conn_src->lock);

	/* insert messages into destination */
	mutex_lock(&conn_dst->lock);
	if (!kdbus_conn_active(conn_dst)) {
		struct kdbus_conn_reply *r, *r_tmp;

		/* our destination connection died, just drop all messages */
		mutex_unlock(&conn_dst->lock);
		list_for_each_entry_safe(q, q_tmp, &msg_list, entry)
			kdbus_queue_entry_free(q);
		list_for_each_entry_safe(r, r_tmp, &reply_list, entry)
			kdbus_conn_reply_free(r);
		return -ECONNRESET;
	}

	list_for_each_entry_safe(q, q_tmp, &msg_list, entry) {
		/* filter messages for a specific name */
		if (name_id > 0 && q->dst_name_id != name_id)
			continue;

		ret = kdbus_pool_move_slice(conn_dst->pool, conn_src->pool,
					    &q->slice);
		if (ret < 0)
			kdbus_queue_entry_free(q);
		else
			kdbus_queue_entry_add(&conn_dst->queue, q);
	}
	list_splice(&reply_list, &conn_dst->reply_list);
	mutex_unlock(&conn_dst->lock);

	/* wake up poll() */
	wake_up_interruptible(&conn_dst->wait);

	return ret;
}

/**
 * kdbus_cmd_conn_info() - retrieve info about a connection
 * @conn:		Connection
 * @cmd_info:		The command as passed in by the ioctl
 * @size:		Size of the passed data structure
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_cmd_conn_info(struct kdbus_conn *conn,
			struct kdbus_cmd_conn_info *cmd_info)
{
	struct kdbus_name_entry *entry = NULL;
	struct kdbus_conn *owner_conn = NULL;
	struct kdbus_conn_info info = {};
	struct kdbus_meta *meta = NULL;
	struct kdbus_pool_slice *slice;
	size_t pos;
	int ret = 0;
	u64 flags;

	if (cmd_info->flags & ~_KDBUS_ATTACH_ALL)
		return -EOPNOTSUPP;

	if (cmd_info->id == 0) {
		const char *name;

		ret = kdbus_items_get_str(cmd_info->items,
					  KDBUS_ITEMS_SIZE(cmd_info, items),
					  KDBUS_ITEM_NAME, &name);
		if (ret < 0)
			return -EINVAL;

		if (!kdbus_name_is_valid(name, false))
			return -EINVAL;

		/* check if 'conn' is allowed to see 'name' */
		ret = kdbus_ep_policy_check_see_access(conn->ep, conn, name);
		if (ret < 0)
			return ret;

		entry = kdbus_name_lock(conn->bus->name_registry, name);
		if (!entry)
			return -ESRCH;
		else if (entry->conn)
			owner_conn = kdbus_conn_ref(entry->conn);
	} else {
		owner_conn = kdbus_bus_find_conn_by_id(conn->bus, cmd_info->id);
		if (!owner_conn) {
			ret = -ENXIO;
			goto exit;
		}

		/* check if 'conn' is allowed to see any of owner_conn's names*/
		ret = kdbus_ep_policy_check_src_names(conn->ep, owner_conn,
						      conn);
		if (ret < 0)
			return ret;
	}

	info.size = sizeof(info);
	info.id = owner_conn->id;
	info.flags = owner_conn->flags;

	/* do not leak domain-specific credentials */
	if (kdbus_meta_ns_eq(conn->meta, owner_conn->meta))
		info.size += owner_conn->meta->size;

	/*
	 * Unlike the rest of the values which are cached at connection
	 * creation time, some values need to be appended here because
	 * at creation time a connection does not have names and other
	 * properties.
	 */
	flags = cmd_info->flags & (KDBUS_ATTACH_NAMES | KDBUS_ATTACH_CONN_NAME);
	if (flags) {
		ret = kdbus_meta_new(&meta);
		if (ret < 0)
			goto exit;

		ret = kdbus_meta_append(meta, owner_conn, 0, flags);
		if (ret < 0)
			goto exit;

		info.size += meta->size;
	}

	ret = kdbus_pool_slice_alloc(conn->pool, &slice, info.size);
	if (ret < 0)
		goto exit;

	ret = kdbus_pool_slice_copy(slice, 0, &info, sizeof(info));
	if (ret < 0)
		goto exit_free;
	pos = sizeof(info);

	if (kdbus_meta_ns_eq(conn->meta, owner_conn->meta)) {
		ret = kdbus_pool_slice_copy(slice, pos, owner_conn->meta->data,
					    owner_conn->meta->size);
		if (ret < 0)
			goto exit_free;

		pos += owner_conn->meta->size;
	}

	if (meta) {
		ret = kdbus_pool_slice_copy(slice, pos, meta->data, meta->size);
		if (ret < 0)
			goto exit_free;
	}

	/* write back the offset */
	cmd_info->offset = kdbus_pool_slice_offset(slice);
	kdbus_pool_slice_flush(slice);
	kdbus_pool_slice_make_public(slice);

exit_free:
	if (ret < 0)
		kdbus_pool_slice_free(slice);

exit:
	kdbus_meta_free(meta);
	kdbus_conn_unref(owner_conn);
	kdbus_name_unlock(conn->bus->name_registry, entry);

	return ret;
}

/**
 * kdbus_cmd_conn_update() - update the attach-flags of a connection or
 *			     the policy entries of a policy holding one
 * @conn:		Connection
 * @cmd:		The command as passed in by the ioctl
 *
 * Return: 0 on success, negative errno on failure.
 */
int kdbus_cmd_conn_update(struct kdbus_conn *conn,
			  const struct kdbus_cmd_update *cmd)
{
	const struct kdbus_item *item;
	bool policy_provided = false;
	bool flags_provided = false;
	u64 attach_flags;
	int ret;

	if (cmd->flags != 0)
		return -EOPNOTSUPP;

	KDBUS_ITEMS_FOREACH(item, cmd->items, KDBUS_ITEMS_SIZE(cmd, items)) {
		switch (item->type) {
		case KDBUS_ITEM_ATTACH_FLAGS:
			/* Only ordinary connections may update their
			 * attach-flags */
			if (!kdbus_conn_is_connected(conn))
				return -EOPNOTSUPP;

			flags_provided = true;
			attach_flags = item->data64[0];
			break;

		case KDBUS_ITEM_NAME:
		case KDBUS_ITEM_POLICY_ACCESS:
			/* Only policy holders may update their policy
			 * entries */
			if (!kdbus_conn_is_policy_holder(conn))
				return -EOPNOTSUPP;

			policy_provided = true;
			break;
		}
	}

	if (policy_provided) {
		ret = kdbus_policy_set(&conn->bus->policy_db, cmd->items,
				       KDBUS_ITEMS_SIZE(cmd, items),
				       1, true, conn);
		if (ret < 0)
			return ret;
	}

	if (flags_provided) {
		mutex_lock(&conn->lock);
		conn->attach_flags = attach_flags;
		mutex_unlock(&conn->lock);
	}

	return 0;
}

/**
 * kdbus_conn_new() - create a new connection
 * @ep:			The endpoint the connection is connected to
 * @hello:		The kdbus_cmd_hello as passed in by the user
 * @meta:		The metadata gathered at open() time of the handle
 * @c:			Returned connection
 *
 * Return: 0 on success, negative errno on failure
 */
int kdbus_conn_new(struct kdbus_ep *ep,
		   struct kdbus_cmd_hello *hello,
		   struct kdbus_meta *meta,
		   struct kdbus_conn **c)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	static struct lock_class_key __key;
#endif
	const struct kdbus_creds *creds = NULL;
	const struct kdbus_item *item;
	const char *conn_name = NULL;
	const char *seclabel = NULL;
	const char *name = NULL;
	struct kdbus_conn *conn;
	struct kdbus_bus *bus = ep->bus;
	size_t seclabel_len = 0;
	bool is_policy_holder;
	bool is_activator;
	bool is_monitor;
	int ret;

	BUG_ON(*c);

	/* Reject unknown flags */
	if (hello->conn_flags & ~(KDBUS_HELLO_ACCEPT_FD |
				  KDBUS_HELLO_ACTIVATOR |
				  KDBUS_HELLO_POLICY_HOLDER |
				  KDBUS_HELLO_MONITOR))
		return -EOPNOTSUPP;

	is_monitor = hello->conn_flags & KDBUS_HELLO_MONITOR;
	is_activator = hello->conn_flags & KDBUS_HELLO_ACTIVATOR;
	is_policy_holder = hello->conn_flags & KDBUS_HELLO_POLICY_HOLDER;

	/* can't be activator or policy holder and monitor at the same time */
	if (is_monitor && (is_activator || is_policy_holder))
		return -EINVAL;

	/* can't be policy holder and activator at the same time */
	if (is_activator && is_policy_holder)
		return -EINVAL;

	/* only privileged connections can activate and monitor */
	if (!kdbus_bus_uid_is_privileged(bus) &&
	    (is_activator || is_policy_holder || is_monitor))
		return -EPERM;

	KDBUS_ITEMS_FOREACH(item, hello->items,
			    KDBUS_ITEMS_SIZE(hello, items)) {
		switch (item->type) {
		case KDBUS_ITEM_NAME:
			if (!is_activator && !is_policy_holder)
				return -EINVAL;

			if (name)
				return -EINVAL;

			if (!kdbus_name_is_valid(item->str, true))
				return -EINVAL;

			name = item->str;
			break;

		case KDBUS_ITEM_CREDS:
			/* privileged processes can impersonate somebody else */
			if (!kdbus_bus_uid_is_privileged(bus))
				return -EPERM;

			if (item->size != KDBUS_ITEM_SIZE(sizeof(*creds)))
				return -EINVAL;

			creds = &item->creds;
			break;

		case KDBUS_ITEM_SECLABEL:
			/* privileged processes can impersonate somebody else */
			if (!kdbus_bus_uid_is_privileged(bus))
				return -EPERM;

			seclabel = item->str;
			seclabel_len = item->size - KDBUS_ITEM_HEADER_SIZE;
			break;

		case KDBUS_ITEM_CONN_NAME:
			/* human-readable connection name (debugging) */
			if (conn_name)
				return -EINVAL;

			conn_name = item->str;
			break;
		}
	}

	if ((is_activator || is_policy_holder) && !name)
		return -EINVAL;

	conn = kzalloc(sizeof(*conn), GFP_KERNEL);
	if (!conn)
		return -ENOMEM;

	if (is_activator || is_policy_holder) {
		/*
		 * Policy holders may install one name, and are
		 * allowed to use wildcards.
		 */
		ret = kdbus_policy_set(&bus->policy_db, hello->items,
				       KDBUS_ITEMS_SIZE(hello, items),
				       1, is_policy_holder, conn);
		if (ret < 0)
			goto exit_free_conn;
	}

	if (conn_name) {
		conn->name = kstrdup(conn_name, GFP_KERNEL);
		if (!conn->name) {
			ret = -ENOMEM;
			goto exit_free_conn;
		}
	}

	kref_init(&conn->kref);
	atomic_set(&conn->active, 0);
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	lockdep_init_map(&conn->dep_map, "s_active", &__key, 0);
#endif
	mutex_init(&conn->lock);
	INIT_LIST_HEAD(&conn->names_list);
	INIT_LIST_HEAD(&conn->names_queue_list);
	INIT_LIST_HEAD(&conn->reply_list);
	atomic_set(&conn->name_count, 0);
	atomic_set(&conn->reply_count, 0);
	INIT_DELAYED_WORK(&conn->work, kdbus_conn_work);
	conn->cred = get_current_cred();
	init_waitqueue_head(&conn->wait);
	kdbus_queue_init(&conn->queue);

	/* init entry, so we can unconditionally remove it */
	INIT_LIST_HEAD(&conn->monitor_entry);

	ret = kdbus_pool_new(conn->name, &conn->pool, hello->pool_size);
	if (ret < 0)
		goto exit_unref_cred;

	ret = kdbus_match_db_new(&conn->match_db);
	if (ret < 0)
		goto exit_free_pool;

	conn->bus = kdbus_bus_ref(ep->bus);
	conn->ep = kdbus_ep_ref(ep);

	/* get new id for this connection */
	conn->id = atomic64_inc_return(&bus->conn_seq_last);

	/* return properties of this connection to the caller */
	hello->bus_flags = bus->bus_flags;
	hello->bloom = bus->bloom;
	hello->id = conn->id;

	BUILD_BUG_ON(sizeof(bus->id128) != sizeof(hello->id128));
	memcpy(hello->id128, bus->id128, sizeof(hello->id128));

	conn->flags = hello->conn_flags;
	conn->attach_flags = hello->attach_flags;

	if (is_activator) {
		u64 flags = KDBUS_NAME_ACTIVATOR;

		ret = kdbus_name_acquire(bus->name_registry, conn,
					 name, &flags, NULL);
		if (ret < 0)
			goto exit_unref_ep;
	}

	if (is_monitor) {
		down_write(&bus->conn_rwlock);
		list_add_tail(&conn->monitor_entry, &bus->monitors_list);
		up_write(&bus->conn_rwlock);
	}

	/* privileged processes can impersonate somebody else */
	if (creds || seclabel) {
		ret = kdbus_meta_new(&conn->owner_meta);
		if (ret < 0)
			goto exit_release_names;

		if (creds) {
			ret = kdbus_meta_append_data(conn->owner_meta,
						     KDBUS_ITEM_CREDS,
						     creds, sizeof(*creds));
			if (ret < 0)
				goto exit_free_meta;
		}

		if (seclabel) {
			ret = kdbus_meta_append_data(conn->owner_meta,
						     KDBUS_ITEM_SECLABEL,
						     seclabel, seclabel_len);
			if (ret < 0)
				goto exit_free_meta;
		}

		/* use the information provided with the HELLO call */
		conn->meta = conn->owner_meta;
	} else {
		/* use the connection's metadata gathered at open() */
		conn->meta = meta;
	}

	/*
	 * Account the connection against the current user (UID), or for
	 * custom endpoints use the anonymous user assigned to the endpoint.
	 */
	if (ep->user) {
		conn->user = kdbus_domain_user_ref(ep->user);
	} else {
		ret = kdbus_domain_get_user(ep->bus->domain,
					    current_fsuid(),
					    &conn->user);
		if (ret < 0)
			goto exit_free_meta;
	}

	/* lock order: domain -> bus -> ep -> names -> conn */
	down_write(&bus->conn_rwlock);
	mutex_lock(&bus->lock);
	mutex_lock(&ep->lock);

	if (bus->disconnected || ep->disconnected) {
		ret = -ESHUTDOWN;
		goto exit_unref_user_unlock;
	}

	if (!kdbus_bus_uid_is_privileged(bus) &&
	    atomic_inc_return(&conn->user->connections) > KDBUS_USER_MAX_CONN) {
		atomic_dec(&conn->user->connections);
		ret = -EMFILE;
		goto exit_unref_user_unlock;
	}

	/* link into bus and endpoint */
	list_add_tail(&conn->ep_entry, &ep->conn_list);
	hash_add(bus->conn_hash, &conn->hentry, conn->id);

	mutex_unlock(&ep->lock);
	mutex_unlock(&bus->lock);
	up_write(&bus->conn_rwlock);

	/* notify subscribers about the new active connection */
	ret = kdbus_notify_id_change(conn->bus, KDBUS_ITEM_ID_ADD,
				     conn->id, conn->flags);
	if (ret < 0) {
		atomic_dec(&conn->user->connections);
		goto exit_domain_user_unref;
	}

	kdbus_notify_flush(conn->bus);

	*c = conn;
	return 0;

exit_unref_user_unlock:
	mutex_unlock(&ep->lock);
	mutex_unlock(&bus->lock);
	up_write(&bus->conn_rwlock);
exit_domain_user_unref:
	kdbus_domain_user_unref(conn->user);
exit_free_meta:
	kdbus_meta_free(conn->owner_meta);
exit_release_names:
	kdbus_name_remove_by_conn(bus->name_registry, conn);
exit_unref_ep:
	kdbus_ep_unref(conn->ep);
	kdbus_bus_unref(conn->bus);
	kdbus_match_db_free(conn->match_db);
exit_free_pool:
	kdbus_pool_free(conn->pool);
exit_unref_cred:
	put_cred(conn->cred);
exit_free_conn:
	kfree(conn->name);
	kfree(conn);

	return ret;
}

/**
 * kdbus_conn_has_name() - check if a connection owns a name
 * @conn:		Connection
 * @name:		Well-know name to check for
 *
 * Return: true if the name is currently owned by the connection
 */
bool kdbus_conn_has_name(struct kdbus_conn *conn, const char *name)
{
	struct kdbus_name_entry *e;
	bool match = false;

	mutex_lock(&conn->lock);
	list_for_each_entry(e, &conn->names_list, conn_entry) {
		if (strcmp(e->name, name) == 0) {
			match = true;
			break;
		}
	}
	mutex_unlock(&conn->lock);

	return match;
}
