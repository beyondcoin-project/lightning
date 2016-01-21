#include "cryptopkt.h"
#include "dns.h"
#include "jsonrpc.h"
#include "lightningd.h"
#include "log.h"
#include "peer.h"
#include "state.h"
#include <ccan/io/io.h>
#include <ccan/list/list.h>
#include <ccan/noerr/noerr.h>
#include <ccan/tal/str/str.h>
#include <ccan/tal/tal.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>

#define FIXME_STUB(peer) do { log_broken((peer)->dstate->base_log, "%s:%u: Implement %s!", __FILE__, __LINE__, __func__); abort(); } while(0)

struct json_connecting {
	/* This owns us, so we're freed after command_fail or command_success */
	struct command *cmd;
	const char *name, *port;
};

/* Send and receive (encrypted) hello message. */
static struct io_plan *peer_test_check(struct io_conn *conn, struct peer *peer)
{
	if (peer->inpkt->pkt_case != PKT__PKT_ERROR)
		fatal("Bad packet type %u", peer->inpkt->pkt_case);
	if (!peer->inpkt->error->problem
	    || strcmp(peer->inpkt->error->problem, "hello") != 0)
		fatal("Bad packet '%.6s'", peer->inpkt->error->problem);
	log_info(peer->log, "Successful hello!");

	/* Sleep forever... */
	return io_wait(conn, peer, io_close_cb, NULL);
}

static struct io_plan *peer_test_read(struct io_conn *conn, struct peer *peer)
{
	return peer_read_packet(conn, peer, peer_test_check);
}

static struct io_plan *peer_test(struct io_conn *conn, struct peer *peer)
{
	Error err = ERROR__INIT;
	Pkt pkt = PKT__INIT;
	pkt.pkt_case = PKT__PKT_ERROR;
	pkt.error = &err;
	err.problem = "hello";
	return peer_write_packet(conn, peer, &pkt, peer_test_read);
}

static void destroy_peer(struct peer *peer)
{
	list_del_from(&peer->dstate->peers, &peer->list);
}

static struct peer *new_peer(struct lightningd_state *dstate,
			     struct io_conn *conn,
			     int addr_type, int addr_protocol,
			     enum state_input offer_anchor,
			     const char *in_or_out)
{
	struct peer *peer = tal(dstate, struct peer);

	assert(offer_anchor == CMD_OPEN_WITH_ANCHOR
	       || offer_anchor == CMD_OPEN_WITHOUT_ANCHOR);

	/* FIXME: Stop listening if too many peers? */
	list_add(&dstate->peers, &peer->list);

	peer->state = STATE_INIT;
	peer->cond = PEER_CMD_OK;
	peer->dstate = dstate;
	peer->addr.type = addr_type;
	peer->addr.protocol = addr_protocol;
	peer->io_data = NULL;
	peer->secrets = NULL;
	list_head_init(&peer->watches);

	peer->us.offer_anchor = offer_anchor;
	if (!seconds_to_rel_locktime(dstate->config.rel_locktime,
				     &peer->us.locktime))
		fatal("Invalid locktime configuration %u",
		      dstate->config.rel_locktime);
	peer->us.mindepth = dstate->config.anchor_confirms;
	/* FIXME: Make this dynamic. */
	peer->us.commit_fee = dstate->config.commitment_fee;

	/* FIXME: Attach IO logging for this peer. */
	tal_add_destructor(peer, destroy_peer);

	peer->addr.addrlen = sizeof(peer->addr.saddr);
	if (getpeername(io_conn_fd(conn), &peer->addr.saddr.s,
			&peer->addr.addrlen) != 0) {
		log_unusual(dstate->base_log,
			    "Could not get address for peer: %s",
			    strerror(errno));
		return tal_free(peer);
	}

	peer->log = new_log(peer, dstate->log_record, "%s%s:%s:",
			    log_prefix(dstate->base_log), in_or_out,
			    netaddr_name(peer, &peer->addr));
	return peer;
}

static struct io_plan *peer_connected_out(struct io_conn *conn,
					  struct lightningd_state *dstate,
					  struct json_connecting *connect)
{
	struct json_result *response;
	struct peer *peer = new_peer(dstate, conn, SOCK_STREAM, IPPROTO_TCP,
				     CMD_OPEN_WITH_ANCHOR, "out");
	if (!peer) {
		command_fail(connect->cmd, "Failed to make peer for %s:%s",
			     connect->name, connect->port);
		return io_close(conn);
	}
	log_info(peer->log, "Connected out to %s:%s",
		 connect->name, connect->port);

	response = new_json_result(connect);	
	json_object_start(response, NULL);
	json_object_end(response);
	command_success(connect->cmd, response);

	return peer_crypto_setup(conn, peer, peer_test);
}

static struct io_plan *peer_connected_in(struct io_conn *conn,
					 struct lightningd_state *dstate)
{
	struct peer *peer = new_peer(dstate, conn, SOCK_STREAM, IPPROTO_TCP,
				     CMD_OPEN_WITHOUT_ANCHOR, "in");
	if (!peer)
		return io_close(conn);

	log_info(peer->log, "Peer connected in");
	return peer_crypto_setup(conn, peer, peer_test);
}

static int make_listen_fd(struct lightningd_state *dstate,
			  int domain, void *addr, socklen_t len)
{
	int fd = socket(domain, SOCK_STREAM, 0);
	if (fd < 0) {
		log_debug(dstate->base_log, "Failed to create %u socket: %s",
			  domain, strerror(errno));
		return -1;
	}

	if (!addr || bind(fd, addr, len) == 0) {
		if (listen(fd, 5) == 0)
			return fd;
		log_unusual(dstate->base_log,
			    "Failed to listen on %u socket: %s",
			    domain, strerror(errno));
	} else
		log_debug(dstate->base_log, "Failed to bind on %u socket: %s",
			  domain, strerror(errno));

	close_noerr(fd);
	return -1;
}

void setup_listeners(struct lightningd_state *dstate, unsigned int portnum)
{
	struct sockaddr_in addr;
	struct sockaddr_in6 addr6;
	socklen_t len;
	int fd1, fd2;
	u16 listen_port;

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(portnum);

	addr6.sin6_family = AF_INET6;
	addr6.sin6_addr = in6addr_any;
	addr6.sin6_port = htons(portnum);

	/* IPv6, since on Linux that (usually) binds to IPv4 too. */
	fd1 = make_listen_fd(dstate, AF_INET6, portnum ? &addr6 : NULL,
			     sizeof(addr6));
	if (fd1 >= 0) {
		struct sockaddr_in6 in6;

		len = sizeof(in6);
		if (getsockname(fd1, (void *)&in6, &len) != 0) {
			log_unusual(dstate->base_log,
				    "Failed get IPv6 sockname: %s",
				    strerror(errno));
			close_noerr(fd1);
		} else {
			addr.sin_port = in6.sin6_port;
			listen_port = ntohs(addr.sin_port);
			log_info(dstate->base_log,
				 "Creating IPv6 listener on port %u",
				 listen_port);
			io_new_listener(dstate, fd1, peer_connected_in, dstate);
		}
	}

	/* Just in case, aim for the same port... */
	fd2 = make_listen_fd(dstate, AF_INET,
			     addr.sin_port ? &addr : NULL, sizeof(addr));
	if (fd2 >= 0) {
		len = sizeof(addr);
		if (getsockname(fd2, (void *)&addr, &len) != 0) {
			log_unusual(dstate->base_log,
				    "Failed get IPv4 sockname: %s",
				    strerror(errno));
			close_noerr(fd2);
		} else {
			listen_port = ntohs(addr.sin_port);
			log_info(dstate->base_log,
				 "Creating IPv4 listener on port %u",
				 listen_port);
			io_new_listener(dstate, fd2, peer_connected_in, dstate);
		}
	}

	if (fd1 < 0 && fd2 < 0)
		fatal("Could not bind to a network address");
}

static void peer_failed(struct lightningd_state *dstate,
			struct json_connecting *connect)
{
	/* FIXME: Better diagnostics! */
	command_fail(connect->cmd, "Failed to connect to peer %s:%s",
		     connect->name, connect->port);
}

static void json_connect(struct command *cmd,
			const char *buffer, const jsmntok_t *params)
{
	struct json_connecting *connect;
	jsmntok_t *host, *port;

	json_get_params(buffer, params, "host", &host, "port", &port, NULL);

	if (!host || !port) {
		command_fail(cmd, "Need host and port");
		return;
	}

	connect = tal(cmd, struct json_connecting);
	connect->cmd = cmd;
	connect->name = tal_strndup(connect, buffer + host->start,
				    host->end - host->start);
	connect->port = tal_strndup(connect, buffer + port->start,
				    port->end - port->start);
	if (!dns_resolve_and_connect(cmd->dstate, connect->name, connect->port,
				     peer_connected_out, peer_failed, connect)) {
		command_fail(cmd, "DNS failed");
		return;
	}
}

const struct json_command connect_command = {
	"connect",
	json_connect,
	"Connect to a {host} at {port}",
	"Returns an empty result on success"
};

struct anchor_watch {
	struct peer *peer;
	enum state_input depthok;
	enum state_input timeout;
	enum state_input unspent;
	enum state_input theyspent;
	enum state_input otherspent;
};

void peer_watch_anchor(struct peer *peer,
		       enum state_input depthok,
		       enum state_input timeout,
		       enum state_input unspent,
		       enum state_input theyspent,
		       enum state_input otherspent)
{
	FIXME_STUB(peer);
}

void peer_unwatch_anchor_depth(struct peer *peer,
			       enum state_input depthok,
			       enum state_input timeout)
{
	FIXME_STUB(peer);
}

void peer_watch_delayed(struct peer *peer,
			const struct bitcoin_tx *tx,
			enum state_input canspend)
{
	FIXME_STUB(peer);
}
void peer_watch_tx(struct peer *peer,
		   const struct bitcoin_tx *tx,
		   enum state_input done)
{
	FIXME_STUB(peer);
}
void peer_watch_close(struct peer *peer,
		      enum state_input done, enum state_input timedout)
{
	FIXME_STUB(peer);
}
void peer_unwatch_close_timeout(struct peer *peer, enum state_input timedout)
{
	FIXME_STUB(peer);
}
bool peer_watch_our_htlc_outputs(struct peer *peer,
				 const struct bitcoin_tx *tx,
				 enum state_input tous_timeout,
				 enum state_input tothem_spent,
				 enum state_input tothem_timeout)
{
	FIXME_STUB(peer);
}
bool peer_watch_their_htlc_outputs(struct peer *peer,
				   const struct bitcoin_event *tx,
				   enum state_input tous_timeout,
				   enum state_input tothem_spent,
				   enum state_input tothem_timeout)
{
	FIXME_STUB(peer);
}
void peer_unwatch_htlc_output(struct peer *peer,
			      const struct htlc *htlc,
			      enum state_input all_done)
{
	FIXME_STUB(peer);
}
void peer_unwatch_all_htlc_outputs(struct peer *peer)
{
	FIXME_STUB(peer);
}
void peer_watch_htlc_spend(struct peer *peer,
			   const struct bitcoin_tx *tx,
			   const struct htlc *htlc,
			   enum state_input done)
{
	FIXME_STUB(peer);
}
void peer_unwatch_htlc_spend(struct peer *peer,
			     const struct htlc *htlc,
			     enum state_input all_done)
{
	FIXME_STUB(peer);
}
void peer_unexpected_pkt(struct peer *peer, const Pkt *pkt)
{
	FIXME_STUB(peer);
}

/* Someone declined our HTLC: details in pkt (we will also get CMD_FAIL) */
void peer_htlc_declined(struct peer *peer, const Pkt *pkt)
{
	FIXME_STUB(peer);
}

/* Called when their update overrides our update cmd. */
void peer_htlc_ours_deferred(struct peer *peer)
{
	FIXME_STUB(peer);
}

/* Successfully added/fulfilled/timedout/routefail an HTLC. */
void peer_htlc_done(struct peer *peer)
{
	FIXME_STUB(peer);
}

/* Someone aborted an existing HTLC. */
void peer_htlc_aborted(struct peer *peer)
{
	FIXME_STUB(peer);
}

/* An on-chain transaction revealed an R value. */
const struct htlc *peer_tx_revealed_r_value(struct peer *peer,
					    const struct bitcoin_event *btc)
{
	FIXME_STUB(peer);
}

bool committed_to_htlcs(const struct peer *peer)
{
	FIXME_STUB(peer);
}

/* Create a bitcoin close tx. */
const struct bitcoin_tx *bitcoin_close(const tal_t *ctx,
				       const struct peer *peer)
{
#if 0
	struct bitcoin_tx *close_tx;
	u8 *redeemscript;

	close_tx = create_close_tx(ctx, peer->us.openpkt, peer->them.openpkt,
				   peer->anchorpkt, 
				   peer->cstate.a.pay_msat / 1000,
				   peer->cstate.b.pay_msat / 1000);

	/* This is what the anchor pays to. */
	redeemscript = bitcoin_redeem_2of2(close_tx, &peer->us.commitkey,
					   &peer->them.commitkey);
	
	/* Combined signatures must validate correctly. */
	if (!check_2of2_sig(close_tx, 0, redeemscript, tal_count(redeemscript),
			    &peer->us.finalkey, &peer->them.finalkey,
			    &peer->us.closesig, &peer->them.closesig))
		fatal("bitcoin_close signature failed");

	/* Create p2sh input for close_tx */
	close_tx->input[0].script = scriptsig_p2sh_2of2(close_tx,
							&peer->us.closesig,
							&peer->them.closesig,
							&peer->us.finalkey,
							&peer->them.finalkey);
	close_tx->input[0].script_length = tal_count(close_tx->input[0].script);

	return close_tx;
#endif
	FIXME_STUB(peer);
}

/* Create a bitcoin spend tx (to spend our commit's outputs) */
const struct bitcoin_tx *bitcoin_spend_ours(const tal_t *ctx,
					    const struct peer *peer)
{
#if 0
	u8 *redeemscript;

	redeemscript = bitcoin_redeem_secret_or_delay(ctx,
						      &peer->us.commitkey,
						      &peer->them.locktime,
						      &peer->them.commitkey,
						      &peer->revocation_hash);

	/* Now, create transaction to spend it. */
	tx = bitcoin_tx(ctx, 1, 1);
	bitcoin_txid(commit, &tx->input[0].txid);
	p2sh_out = find_p2sh_out(commit, redeemscript);
	tx->input[0].index = p2sh_out;
	tx->input[0].input_amount = commit->output[p2sh_out].amount;
	tx->fee = fee;

	tx->input[0].sequence_number = bitcoin_nsequence(locktime);

	if (commit->output[p2sh_out].amount <= fee)
		errx(1, "Amount of %llu won't exceed fee",
		     (unsigned long long)commit->output[p2sh_out].amount);

	tx->output[0].amount = commit->output[p2sh_out].amount - fee;
	tx->output[0].script = scriptpubkey_p2sh(tx,
						 bitcoin_redeem_single(tx, &outpubkey));
	tx->output[0].script_length = tal_count(tx->output[0].script);

	/* Now get signature, to set up input script. */
	if (!sign_tx_input(tx, 0, redeemscript, tal_count(redeemscript),
			   &privkey, &pubkey1, &sig.sig))
		errx(1, "Could not sign tx");
	sig.stype = SIGHASH_ALL;
	tx->input[0].script = scriptsig_p2sh_secret(tx, NULL, 0, &sig,
						    redeemscript,
						    tal_count(redeemscript));
	tx->input[0].script_length = tal_count(tx->input[0].script);
#endif
	FIXME_STUB(peer);
}

/* Create a bitcoin spend tx (to spend their commit's outputs) */
const struct bitcoin_tx *bitcoin_spend_theirs(const tal_t *ctx,
					      const struct peer *peer,
					      const struct bitcoin_event *btc)
{
	FIXME_STUB(peer);
}

/* Create a bitcoin steal tx (to steal all their commit's outputs) */
const struct bitcoin_tx *bitcoin_steal(const tal_t *ctx,
				       const struct peer *peer,
				       struct bitcoin_event *btc)
{
	FIXME_STUB(peer);
}

/* Create our commit tx */
const struct bitcoin_tx *bitcoin_commit(const tal_t *ctx, struct peer *peer)
{
	FIXME_STUB(peer);
}

/* Create a HTLC refund collection */
const struct bitcoin_tx *bitcoin_htlc_timeout(const tal_t *ctx,
					      const struct peer *peer,
					      const struct htlc *htlc)
{
	FIXME_STUB(peer);
}

/* Create a HTLC collection */
const struct bitcoin_tx *bitcoin_htlc_spend(const tal_t *ctx,
					    const struct peer *peer,
					    const struct htlc *htlc)
{
	FIXME_STUB(peer);
}

/* Start creation of the bitcoin anchor tx. */
void bitcoin_create_anchor(struct peer *peer, enum state_input done)
{
	FIXME_STUB(peer);
}

/* We didn't end up broadcasting the anchor: release the utxos.
 * If done != INPUT_NONE, remove existing create_anchor too. */
void bitcoin_release_anchor(struct peer *peer, enum state_input done)
{
	FIXME_STUB(peer);
}

/* Get the bitcoin anchor tx. */
const struct bitcoin_tx *bitcoin_anchor(const tal_t *ctx, struct peer *peer)
{
	FIXME_STUB(peer);
}

/* FIXME: Somehow we should show running DNS lookups! */
/* FIXME: Show status of peers! */
static void json_getpeers(struct command *cmd,
			  const char *buffer, const jsmntok_t *params)
{
	struct peer *p;
	struct json_result *response = new_json_result(cmd);	

	json_object_start(response, NULL);
	json_array_start(response, "peers");
	list_for_each(&cmd->dstate->peers, p, list) {
		json_object_start(response, NULL);
		json_add_string(response, "name", log_prefix(p->log));
		json_add_hex(response, "id", p->id.der, pubkey_derlen(&p->id));
		json_object_end(response);
	}
	json_array_end(response);
	json_object_end(response);
	command_success(cmd, response);
}

const struct json_command getpeers_command = {
	"getpeers",
	json_getpeers,
	"List the current peers",
	"Returns a 'peers' array"
};
