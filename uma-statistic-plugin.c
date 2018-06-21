#include <lib.h>
#include <mail-storage-private.h>
#include <stdio.h>
#include <stdlib.h>
#include <tcrdb.h>
#include "../plugin-utils/plugin-utils.h"
#include "uma-statistic-plugin.h"


#define DB_LOCKFILE "/var/run/dovecot-statistic/statistic-db.lck"
#ifndef DB_SERVER_PORT
	#define DB_SERVER_PORT 9090
#endif
#ifndef DB_SERVER_ADDR
	#define DB_SERVER_ADDR "localhost"
#endif
#define DOVECOT_PLUGIN_ENV_WRITE_SYSLOG "uma_statistic_syslog"
#define DOVECOT_PLUGIN_ENV_WRITE_DB "uma_statistic_db"
#define DOVECOT_PLUGIN_ENV_IGNORE_FOLDER_LIST "uma_statistic_ignore_folder_list"
#define DB_KEY_PREFIX "/dovecot-statistic"
#define DB_GLOBAL_KEY_PREFIX "/dovecot-statistic-global"
#define DB_KEY_FILE_MESSAGES "messages"
#define DB_KEY_FILE_VSIZE "vsize"

static MODULE_CONTEXT_DEFINE_INIT(uma_statistic_mail_user_module, &mail_user_module_register);
static MODULE_CONTEXT_DEFINE_INIT(uma_statistic_mail_storage_module, &mail_storage_module_register);
static MODULE_CONTEXT_DEFINE_INIT(uma_statistic_mail_module, &mail_module_register);

struct uma_statistic_user_context {
	union mail_user_module_context module_ctx;

	int write_syslog;
	int write_db;
	char *ignore_folder_list;

	char *spuser;
};

struct uma_statistic_mailbox_context {
	union mailbox_module_context module_ctx;

	int size_changed;
	int32_t messages_diff;
};

enum statistic_type {
	STATISTIC_TYPE_MESSAGES,
	STATISTIC_TYPE_VSIZE
};

static bool uma_statistic_tcrdbput_llu2string_ext(TCRDB *rdb, const void *kbuf, int ksiz, unsigned long long num)
{
	char buffer[24];

	snprintf(buffer, 24, "%llu", num);

	return tcrdbput(rdb, kbuf, ksiz, buffer, strlen(buffer));
}

static unsigned long long uma_statistic_tcrdbget_string2llu_ext(TCRDB *rdb, const void *kbuf, int ksiz)
{
	unsigned long long num;
	char *buf;
	char *endptr;
	int buf_size;

	buf = tcrdbget(rdb, kbuf, ksiz, &buf_size);
	if (buf) {
		num = strtoull(buf, &endptr, 10);
		free(buf);
	} else {
		num = 0;
	}

	return num;
}

static int uma_statistic_db_setlock()
{
	int db_lockfile;
	struct flock db_lock;

	db_lock.l_type = F_WRLCK;
	db_lock.l_start = 0;
	db_lock.l_whence = SEEK_SET;
	db_lock.l_len = 0;
	db_lock.l_pid = getpid();

	db_lockfile = open(DB_LOCKFILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	if (db_lockfile) {
		fcntl(db_lockfile, F_SETLKW, &db_lock);
	}

	return db_lockfile;
}

static void uma_statistic_plugin_db_releaselock(int db_lockfile)
{
	struct flock db_lock;

	db_lock.l_type = F_UNLCK;
	db_lock.l_start = 0;
	db_lock.l_whence = SEEK_SET;
	db_lock.l_len = 0;
	db_lock.l_pid = getpid();
	fcntl(db_lockfile, F_SETLKW, &db_lock);

	close(db_lockfile);
}

static char *uma_statistic_mailbox_db_key_get(const char *username, const char *mailbox_name, enum statistic_type type, size_t *db_key_len)
{
	char *db_key;
	size_t key_len;
	const char *type_name;

	switch (type) {
	case STATISTIC_TYPE_MESSAGES:
		type_name = DB_KEY_FILE_MESSAGES;
		break;
	case STATISTIC_TYPE_VSIZE:
		type_name = DB_KEY_FILE_VSIZE;
		break;
	default:
		return NULL;
	}
	key_len = strlen(DB_KEY_PREFIX) + strlen(username) + strlen(mailbox_name) + strlen(type_name) + 4;

	db_key = malloc(key_len);
	snprintf(db_key, key_len, "%s/%s/%s/%s", DB_KEY_PREFIX, username, mailbox_name, type_name);

	if (db_key_len) {
		*db_key_len = key_len - 1;
	}
	return db_key;
}

static char *uma_statistic_mailbox_db_global_key_get(enum statistic_type type, size_t *db_key_len)
{
	char *db_key;
	size_t key_len;
	const char *type_name;

	switch (type) {
	case STATISTIC_TYPE_MESSAGES:
		type_name = DB_KEY_FILE_MESSAGES;
		break;
	case STATISTIC_TYPE_VSIZE:
		type_name = DB_KEY_FILE_VSIZE;
		break;
	default:
		return NULL;
	}
	key_len = strlen(DB_GLOBAL_KEY_PREFIX) + strlen(type_name) + 2;

	db_key = malloc(key_len);
	snprintf(db_key, key_len, "%s/%s", DB_GLOBAL_KEY_PREFIX, type_name);

	if (db_key_len) {
		*db_key_len = key_len - 1;
	}
	return db_key;
}

static void uma_statistic_mailbox_status_save(struct mailbox *mailbox, int32_t messages_diff)
{
	struct uma_statistic_user_context *user_ctx = NULL;

	user_ctx = MODULE_CONTEXT(mailbox->storage->user, uma_statistic_mail_user_module);

	if (!user_ctx->spuser) {
		return;
	}
	if (!plugin_utils_isin_list(user_ctx->ignore_folder_list, mailbox->name, 1)) {
		enum mailbox_status_items status_items;
		enum mailbox_status_items metadata_items;
		struct mailbox_status status;
		struct mailbox_metadata metadata;
		uint32_t messages;
		uint64_t virtual_size;

		status_items = STATUS_MESSAGES;
		metadata_items = MAILBOX_METADATA_VIRTUAL_SIZE;
		mailbox_get_status(mailbox, status_items, &status);
		mailbox_get_metadata(mailbox, metadata_items, &metadata);
		messages = status.messages + messages_diff;
		virtual_size = metadata.virtual_size;

		if (user_ctx->write_syslog) {
			i_info("uma_statistic: user: %s, mailbox: %s, messages: %u, vsize: %llu", user_ctx->spuser, mailbox->name, messages, (unsigned long long) virtual_size);
		}

		if (user_ctx->write_db) {
			int db_lockfile;

			db_lockfile = uma_statistic_db_setlock();
			if (db_lockfile) {
				TCRDB *rdb = NULL;

				rdb = tcrdbnew();
				if (!tcrdbopen(rdb, DB_SERVER_ADDR, DB_SERVER_PORT)) {
					i_warning("uma_statistic: warning: can not open statistic database, saving statistic to database is disabled");
				} else {
					char *key_messages = NULL;
					size_t key_messages_len;
					char *key_vsize = NULL;
					size_t key_vsize_len;
					char *global_key_messages = NULL;
					size_t global_key_messages_len;
					char *global_key_vsize = NULL;
					size_t global_key_vsize_len;
					unsigned long long messages_old;
					unsigned long long vsize_old;
					unsigned long long global_messages;
					unsigned long long global_vsize;

					key_messages = uma_statistic_mailbox_db_key_get(user_ctx->spuser, mailbox->name, STATISTIC_TYPE_MESSAGES, &key_messages_len);
					key_vsize = uma_statistic_mailbox_db_key_get(user_ctx->spuser, mailbox->name, STATISTIC_TYPE_VSIZE, &key_vsize_len);
					global_key_messages = uma_statistic_mailbox_db_global_key_get(STATISTIC_TYPE_MESSAGES, &global_key_messages_len);
					global_key_vsize = uma_statistic_mailbox_db_global_key_get(STATISTIC_TYPE_VSIZE, &global_key_vsize_len);

					messages_old = uma_statistic_tcrdbget_string2llu_ext(rdb, key_messages, key_messages_len);
					vsize_old = uma_statistic_tcrdbget_string2llu_ext(rdb, key_vsize, key_vsize_len);
					global_messages = uma_statistic_tcrdbget_string2llu_ext(rdb, global_key_messages, global_key_messages_len);
					global_vsize = uma_statistic_tcrdbget_string2llu_ext(rdb, global_key_vsize, global_key_vsize_len);

					if (global_messages == 0) {
						i_warning("uma_statistic: warning: global messages entry was not found. Use 0 as initial value.");
					}
					if (global_vsize == 0) {
						i_warning("uma_statistic: warning: global virtual size entry was not found. Use 0 as initial value.");
					}

					global_messages += (messages - messages_old);
					global_vsize += (virtual_size - vsize_old);

					uma_statistic_tcrdbput_llu2string_ext(rdb, global_key_messages, global_key_messages_len, global_messages);
					uma_statistic_tcrdbput_llu2string_ext(rdb, global_key_vsize, global_key_vsize_len, global_vsize);
					uma_statistic_tcrdbput_llu2string_ext(rdb, key_messages, key_messages_len, (long long unsigned) messages);
					uma_statistic_tcrdbput_llu2string_ext(rdb, key_vsize, key_vsize_len, virtual_size);

					free(key_messages);
					free(key_vsize);
					free(global_key_messages);
					free(global_key_vsize);

					tcrdbclose(rdb);
					tcrdbdel(rdb);

					uma_statistic_plugin_db_releaselock(db_lockfile);
				}
			} else {
				i_error("uma_statistic: ERROR: Can not create lockfile");
			}
		}
	}
}

static void uma_statistic_mailbox_status_delete(struct mailbox *mailbox)
{
	struct uma_statistic_user_context *user_ctx = NULL;

	user_ctx = MODULE_CONTEXT(mailbox->storage->user, uma_statistic_mail_user_module);

	if (!user_ctx->spuser) {
		return;
	}
	if (!plugin_utils_isin_list(user_ctx->ignore_folder_list, mailbox->name, 1)) {
		if (user_ctx->write_syslog) {
			i_info("uma_statistic: user: %s, mailbox: %s, deleted", user_ctx->spuser, mailbox->name);
			i_info("uma_statistic: user: %s, mailbox: %s, messages: 0, vsize: 0", user_ctx->spuser, mailbox->name);
		}

		if (user_ctx->write_db) {
			int db_lockfile;

			db_lockfile = uma_statistic_db_setlock();
			if (db_lockfile) {
				TCRDB *rdb = NULL;

				rdb = tcrdbnew();
				if (!tcrdbopen(rdb, DB_SERVER_ADDR, DB_SERVER_PORT)) {
					i_warning("uma_statistic: warning: can not open statistic database, saving statistic to database is disabled");
				} else {
					char *key_messages = NULL;
					size_t key_messages_len;
					char *key_vsize = NULL;
					size_t key_vsize_len;
					char *global_key_messages = NULL;
					size_t global_key_messages_len;
					char *global_key_vsize = NULL;
					size_t global_key_vsize_len;
					unsigned long long messages;
					unsigned long long vsize;
					unsigned long long global_messages;
					unsigned long long global_vsize;

					key_messages = uma_statistic_mailbox_db_key_get(user_ctx->spuser, mailbox->name, STATISTIC_TYPE_MESSAGES, &key_messages_len);
					key_vsize = uma_statistic_mailbox_db_key_get(user_ctx->spuser, mailbox->name, STATISTIC_TYPE_VSIZE, &key_vsize_len);
					global_key_messages = uma_statistic_mailbox_db_global_key_get(STATISTIC_TYPE_MESSAGES, &global_key_messages_len);
					global_key_vsize = uma_statistic_mailbox_db_global_key_get(STATISTIC_TYPE_VSIZE, &global_key_vsize_len);


					messages = uma_statistic_tcrdbget_string2llu_ext(rdb, key_messages, key_messages_len);
					vsize = uma_statistic_tcrdbget_string2llu_ext(rdb, key_vsize, key_vsize_len);
					global_messages = uma_statistic_tcrdbget_string2llu_ext(rdb, global_key_messages, global_key_messages_len);
					global_vsize = uma_statistic_tcrdbget_string2llu_ext(rdb, global_key_vsize, global_key_vsize_len);

					global_messages -= messages;
					global_vsize -= vsize;

					uma_statistic_tcrdbput_llu2string_ext(rdb, global_key_messages, global_key_messages_len, global_messages);
					uma_statistic_tcrdbput_llu2string_ext(rdb, global_key_vsize, global_key_vsize_len, global_vsize);

					tcrdbout(rdb, key_messages, key_messages_len);
					tcrdbout(rdb, key_vsize, key_vsize_len);

					uma_statistic_plugin_db_releaselock(db_lockfile);

					free(global_key_messages);
					free(global_key_vsize);
					free(key_messages);
					free(key_vsize);

					tcrdbclose(rdb);
					tcrdbdel(rdb);
				}
			} else {
				i_error("uma_statistic: ERROR: Can not create lockfile");
			}
		}
	}
}

static void uma_statistic_user_deinit(struct mail_user *user)
{
	struct uma_statistic_user_context *user_ctx = NULL;

	user_ctx = MODULE_CONTEXT(user, uma_statistic_mail_user_module);

	free(user_ctx->ignore_folder_list);
	free(user_ctx->spuser);

	user_ctx->module_ctx.super.deinit(user);
}

static void uma_statistic_mailbox_close(struct mailbox *box)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;

	mailbox_ctx = MODULE_CONTEXT(box, uma_statistic_mail_storage_module);

	if (mailbox_ctx->size_changed) {
		uma_statistic_mailbox_status_save(box, mailbox_ctx->messages_diff);
	}

	mailbox_ctx->module_ctx.super.close(box);
}

static void uma_statistic_mailbox_free(struct mailbox *box)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;

	mailbox_ctx = MODULE_CONTEXT(box, uma_statistic_mail_storage_module);

	mailbox_ctx->module_ctx.super.free(box);
}

static int uma_statistic_mailbox_delete(struct mailbox *box)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;
	int ret;

	mailbox_ctx = MODULE_CONTEXT(box, uma_statistic_mail_storage_module);

	if ((ret = mailbox_ctx->module_ctx.super.delete_box(box)) >= 0) {
		uma_statistic_mailbox_status_delete(box);
	}

	return ret;
}

static int uma_statistic_mailbox_rename(struct mailbox *src, struct mailbox *dest)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;
	int ret;

	mailbox_ctx = MODULE_CONTEXT(src, uma_statistic_mail_storage_module);

	if ((ret = mailbox_ctx->module_ctx.super.rename_box(src, dest)) >= 0) {
		uma_statistic_mailbox_status_delete(src);
		uma_statistic_mailbox_status_save(dest, 0);
	}

	return ret;
}

static int uma_statistic_mailbox_save_finish(struct mail_save_context *ctx)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;
	int ret;

	mailbox_ctx = MODULE_CONTEXT(ctx->transaction->box, uma_statistic_mail_storage_module);

	if ((ret = mailbox_ctx->module_ctx.super.save_finish(ctx)) >= 0) {
		mailbox_ctx->size_changed = 1;
		mailbox_ctx->messages_diff += 1;
	}

	return ret;
}

static int uma_statistic_mailbox_copy(struct mail_save_context *ctx, struct mail *mail)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;
	int ret;

	mailbox_ctx = MODULE_CONTEXT(ctx->transaction->box, uma_statistic_mail_storage_module);

	if ((ret = mailbox_ctx->module_ctx.super.copy(ctx, mail)) >= 0) {
		const char *spuser_src;

		spuser_src = plugin_utils_get_spuser(mail->box->storage->user);

		/* dovecot-lda also calls copy during saving mails, but the spuser feld of the source mail is NULL */
		if (!plugin_utils_is_dovecot_lda_copy(mail, spuser_src)) {
			mailbox_ctx->size_changed = 1;
			mailbox_ctx->messages_diff += 1;
		}
	}

	return ret;
}

static void uma_statistic_mail_expunge(struct mail *mail)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;
	union mail_module_context *mail_ctx = NULL;
	struct mail_private *mail_private = NULL;

	mail_private = (struct mail_private *) mail;
	mailbox_ctx = MODULE_CONTEXT(mail->box, uma_statistic_mail_storage_module);
	mail_ctx = MODULE_CONTEXT(mail_private, uma_statistic_mail_module);

	mailbox_ctx->size_changed = 1;

	mail_ctx->super.expunge(mail);
}

static void uma_statistic_mail_user_created(struct mail_user *user)
{
	struct uma_statistic_user_context *user_ctx = NULL;
	struct mail_user_vfuncs *v = NULL;
	const char *dovecot_config = NULL;

	v = user->vlast;
	user_ctx = p_new(user->pool, struct uma_statistic_user_context, 1);
	user_ctx->module_ctx.super = *v;
	user->vlast = &user_ctx->module_ctx.super;

	v->deinit = uma_statistic_user_deinit;

	dovecot_config = mail_user_plugin_getenv(user, DOVECOT_PLUGIN_ENV_WRITE_SYSLOG);
	if (dovecot_config && (!strcmp(dovecot_config, "1") || !strcmp(dovecot_config, "yes"))) {
		user_ctx->write_syslog = 1;
	} else {
		user_ctx->write_syslog = 0;
	}
	dovecot_config = mail_user_plugin_getenv(user, DOVECOT_PLUGIN_ENV_WRITE_DB);
	if (dovecot_config && (!strcmp(dovecot_config, "1") || !strcmp(dovecot_config, "yes"))) {
		user_ctx->write_db = 1;
	} else {
		user_ctx->write_db = 0;
	}
	dovecot_config = mail_user_plugin_getenv(user, DOVECOT_PLUGIN_ENV_IGNORE_FOLDER_LIST);
	if (dovecot_config) {
		user_ctx->ignore_folder_list = strdup(dovecot_config);
	} else {
		user_ctx->ignore_folder_list = NULL;
	}
	dovecot_config = plugin_utils_get_spuser(user);
	if (dovecot_config) {
		user_ctx->spuser = strdup(dovecot_config);
	} else {
		user_ctx->spuser = NULL;
	}

	MODULE_CONTEXT_SET(user, uma_statistic_mail_user_module, user_ctx);
}

static void uma_statistic_mailbox_allocated(struct mailbox *box)
{
	struct uma_statistic_mailbox_context *mailbox_ctx = NULL;
	struct mailbox_vfuncs *v = NULL;

	v = box->vlast;
	mailbox_ctx = p_new(box->pool, struct uma_statistic_mailbox_context, 1);
	mailbox_ctx->module_ctx.super = *v;
	box->vlast = &mailbox_ctx->module_ctx.super;

	v->close = uma_statistic_mailbox_close;
	v->free = uma_statistic_mailbox_free;
	v->delete_box = uma_statistic_mailbox_delete;
	v->rename_box = uma_statistic_mailbox_rename;
	v->save_finish = uma_statistic_mailbox_save_finish;
	v->copy = uma_statistic_mailbox_copy;

	mailbox_ctx->size_changed = 0;
	mailbox_ctx->messages_diff = 0;

	MODULE_CONTEXT_SET(box, uma_statistic_mail_storage_module, mailbox_ctx);
}

static void uma_statistic_mail_allocated(struct mail *mail)
{
	union mail_module_context *mail_ctx;
	struct mail_vfuncs *v = NULL;
	struct mail_private *mail_private = NULL;

	mail_private = (struct mail_private *) mail;
	v = mail_private->vlast;
	mail_ctx = p_new(mail_private->pool, union mail_module_context, 1);
	mail_ctx->super = *v;
	mail_private->vlast = &mail_ctx->super;

	v->expunge = uma_statistic_mail_expunge;

	MODULE_CONTEXT_SET_SELF(mail_private, uma_statistic_mail_module, mail_ctx);
}

static struct mail_storage_hooks uma_statistic_mail_storage_hooks = {
	.mail_user_created = uma_statistic_mail_user_created,
	.mailbox_allocated = uma_statistic_mailbox_allocated,
	.mail_allocated = uma_statistic_mail_allocated
};

void uma_statistic_plugin_init(struct module *module)
{
	mail_storage_hooks_add(module, &uma_statistic_mail_storage_hooks);
}

void uma_statistic_plugin_deinit(void)
{
	mail_storage_hooks_remove(&uma_statistic_mail_storage_hooks);
}

#ifdef DOVECOT_ABI_VERSION
 const char *uma_statistic_plugin_version = DOVECOT_ABI_VERSION;
#else
 const char *uma_statistic_plugin_version = DOVECOT_VERSION;
#endif

const char *mail_uma_statistic_plugin_dependencies[] = { NULL };
