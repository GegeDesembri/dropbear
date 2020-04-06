/*
 * Dropbear SSH
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * Copyright (c) 2004 by Mihnea Stoenescu
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include "includes.h"
#include "buffer.h"
#include "dbutil.h"
#include "session.h"
#include "ssh.h"
#include "runopts.h"
#include "auth.h"
#include "agentfwd.h"

#if DROPBEAR_CLI_PUBKEY_AUTH
static void send_msg_userauth_pubkey(sign_key *key, enum signkey_type sigtype, int realsign);

/* Called when we receive a SSH_MSG_USERAUTH_FAILURE for a pubkey request.
 * We use it to remove the key we tried from the list */
void cli_pubkeyfail() {
	m_list_elem *iter;
	for (iter = cli_opts.privkeys->first; iter; iter = iter->next) {
		sign_key *iter_key = (sign_key*)iter->item;
		
		if (iter_key == cli_ses.lastprivkey)
		{
			/* found the failing key */
			list_remove(iter);
			sign_key_free(iter_key);
			cli_ses.lastprivkey = NULL;
			return;
		}
	}
}

void recv_msg_userauth_pk_ok() {
	m_list_elem *iter;
	buffer* keybuf = NULL;
	char* algotype = NULL;
	unsigned int algolen;
	enum signkey_type sigtype, keytype;
	unsigned int remotelen;

	TRACE(("enter recv_msg_userauth_pk_ok"))

	algotype = buf_getstring(ses.payload, &algolen);
	sigtype = signature_type_from_name(algotype, algolen);
	keytype = signkey_type_from_signature(sigtype);
	TRACE(("recv_msg_userauth_pk_ok: type %d", sigtype))
	m_free(algotype);

	keybuf = buf_new(MAX_PUBKEY_SIZE);

	remotelen = buf_getint(ses.payload);

	/* Iterate through our keys, find which one it was that matched, and
	 * send a real request with that key */
	for (iter = cli_opts.privkeys->first; iter; iter = iter->next) {
		sign_key *key = (sign_key*)iter->item;
		if (key->type != keytype) {
			/* Types differed */
			TRACE(("types differed"))
			continue;
		}

		/* Now we compare the contents of the key */
		keybuf->pos = keybuf->len = 0;
		buf_put_pub_key(keybuf, key, keytype);
		buf_setpos(keybuf, 0);
		buf_incrpos(keybuf, 4); /* first int is the length of the remainder (ie
								   remotelen) which has already been taken from
								   the remote buffer */


		if (keybuf->len-4 != remotelen) {
			TRACE(("lengths differed: localh %d remote %d", keybuf->len, remotelen))
			/* Lengths differed */
			continue;
		}
		if (memcmp(buf_getptr(keybuf, remotelen),
					buf_getptr(ses.payload, remotelen), remotelen) != 0) {
			/* Data didn't match this key */
			TRACE(("data differed"))
			continue;
		}

		/* Success */
		break;
	}
	buf_free(keybuf);

	if (iter != NULL) {
		TRACE(("matching key"))
		/* XXX TODO: if it's an encrypted key, here we ask for their
		 * password */
		send_msg_userauth_pubkey((sign_key*)iter->item, keytype, 1);
	} else {
		TRACE(("That was whacky. We got told that a key was valid, but it didn't match our list. Sounds like dodgy code on Dropbear's part"))
	}
	
	TRACE(("leave recv_msg_userauth_pk_ok"))
}

static void cli_buf_put_sign(buffer* buf, sign_key *key, enum signkey_type sigtype,
			const buffer *data_buf) {
#if DROPBEAR_CLI_AGENTFWD
	// TODO: rsa-sha256 agent
	if (key->source == SIGNKEY_SOURCE_AGENT) {
		/* Format the agent signature ourselves, as buf_put_sign would. */
		buffer *sigblob;
		sigblob = buf_new(MAX_PUBKEY_SIZE);
		agent_buf_sign(sigblob, key, data_buf);
		buf_putbufstring(buf, sigblob);
		buf_free(sigblob);
	} else 
#endif /* DROPBEAR_CLI_AGENTFWD */
	{
		buf_put_sign(buf, key, sigtype, data_buf);
	}
}

static void send_msg_userauth_pubkey(sign_key *key, enum signkey_type sigtype, int realsign) {

	const char *algoname = NULL;
	unsigned int algolen;
	buffer* sigbuf = NULL;
	enum signkey_type keytype = signkey_type_from_signature(sigtype);

	TRACE(("enter send_msg_userauth_pubkey"))
	CHECKCLEARTOWRITE();

	buf_putbyte(ses.writepayload, SSH_MSG_USERAUTH_REQUEST);

	buf_putstring(ses.writepayload, cli_opts.username,
			strlen(cli_opts.username));

	buf_putstring(ses.writepayload, SSH_SERVICE_CONNECTION,
			SSH_SERVICE_CONNECTION_LEN);

	buf_putstring(ses.writepayload, AUTH_METHOD_PUBKEY,
			AUTH_METHOD_PUBKEY_LEN);

	buf_putbyte(ses.writepayload, realsign);

	algoname = signature_name_from_type(sigtype, &algolen);
	buf_putstring(ses.writepayload, algoname, algolen);
	buf_put_pub_key(ses.writepayload, key, keytype);

	if (realsign) {
		TRACE(("realsign"))
		/* We put the signature as well - this contains string(session id), then
		 * the contents of the write payload to this point */
		sigbuf = buf_new(4 + ses.session_id->len + ses.writepayload->len);
		buf_putbufstring(sigbuf, ses.session_id);
		buf_putbytes(sigbuf, ses.writepayload->data, ses.writepayload->len);
		cli_buf_put_sign(ses.writepayload, key, sigtype, sigbuf);
		buf_free(sigbuf); /* Nothing confidential in the buffer */
	}

	encrypt_packet();
	TRACE(("leave send_msg_userauth_pubkey"))
}

/* Returns 1 if a key was tried */
int cli_auth_pubkey() {

	TRACE(("enter cli_auth_pubkey"))

#if DROPBEAR_CLI_AGENTFWD
	if (!cli_opts.agent_keys_loaded) {
		/* get the list of available keys from the agent */
		cli_load_agent_keys(cli_opts.privkeys);
		cli_opts.agent_keys_loaded = 1;
	}
#endif

	if (cli_opts.privkeys->first) {
		sign_key * key = (sign_key*)cli_opts.privkeys->first->item;
		enum signkey_type sigtype = key->type;
		/* Send a trial request */
#if DROPBEAR_RSA && DROPBEAR_RSA_SHA256
		// TODO: use ext-info to choose rsa kind
		if (sigtype == DROPBEAR_SIGNKEY_RSA) {
			sigtype = DROPBEAR_SIGNKEY_RSA_SHA256;
		}
#endif
		send_msg_userauth_pubkey(key, sigtype, 0);
		cli_ses.lastprivkey = key;
		TRACE(("leave cli_auth_pubkey-success"))
		return 1;
	} else {
		/* no more keys left */
		TRACE(("leave cli_auth_pubkey-failure"))
		return 0;
	}
}

void cli_auth_pubkey_cleanup() {

#if DROPBEAR_CLI_AGENTFWD
	m_close(cli_opts.agent_fd);
	cli_opts.agent_fd = -1;
#endif

	while (cli_opts.privkeys->first) {
		sign_key * key = list_remove(cli_opts.privkeys->first);
		sign_key_free(key);
	}
}
#endif /* Pubkey auth */
