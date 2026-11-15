/* The patched libnice socket is internal, so exercise it here with a fake base. */
#include "tls.c"

#include <assert.h>
#include <string.h>

static gboolean fake_eof = FALSE;
static gboolean fake_send_failure = FALSE;
static unsigned fake_reads = 0;
gssize nice_socket_recv(NiceSocket *s, NiceAddress *a, gsize n, gchar *b) {
    (void)s;
    (void)a;
    (void)n;
    (void)b;
    ++fake_reads;
    return fake_eof ? -1 : 0;
}
gssize nice_socket_send_reliable(NiceSocket *s, const NiceAddress *a, gsize n, const gchar *b) {
    (void)s;
    (void)a;
    (void)b;
    return fake_send_failure ? -1 : (gssize)n;
}
gboolean nice_socket_is_reliable(NiceSocket *s) {
    (void)s;
    return TRUE;
}
gboolean nice_socket_can_send(NiceSocket *s, NiceAddress *a) {
    (void)s;
    (void)a;
    return TRUE;
}
void nice_socket_set_writable_callback(NiceSocket *s, NiceSocketWritableCb cb, gpointer p) {
    (void)s;
    (void)cb;
    (void)p;
}
gboolean nice_socket_is_based_on(NiceSocket *s, NiceSocket *o) { return s == o; }
void nice_socket_free(NiceSocket *s) { (void)s; }
gsize output_message_get_size(const NiceOutputMessage *message) {
    gsize total = 0;
    for (gint i = 0; i < message->n_buffers; ++i)
        total += message->buffers[i].size;
    return total;
}
void nice_socket_queue_send(GQueue *q, const NiceAddress *a, const NiceOutputMessage *m, guint n) {
    (void)a;
    (void)m;
    for (guint i = 0; i < n; ++i)
        g_queue_push_tail(q, g_malloc(1));
}
void nice_socket_free_send_queue(GQueue *q) { g_queue_clear_full(q, g_free); }
void nice_socket_flush_send_queue(NiceSocket *s, GQueue *q) {
    (void)s;
    nice_socket_free_send_queue(q);
}

static void transfer(SSL *source, SSL *target) {
    unsigned char bytes[16384];
    BIO *output = SSL_get_wbio(source);
    while (BIO_ctrl_pending(output)) {
        const int n = BIO_read(output, bytes, sizeof(bytes));
        assert(n > 0 && BIO_write(SSL_get_rbio(target), bytes, n) == n);
    }
}

static SSL *make_ssl(SSL_CTX *context, gboolean server) {
    SSL *ssl = SSL_new(context);
    BIO *input = BIO_new(BIO_s_mem());
    BIO *output = BIO_new(BIO_s_mem());
    assert(ssl && input && output);
    BIO_set_mem_eof_return(input, -1);
    SSL_set_bio(ssl, input, output);
    if (server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);
    return ssl;
}

static void do_handshake(SSL *client, SSL *server) {
    for (unsigned i = 0; i < 20 && (!SSL_is_init_finished(client) || !SSL_is_init_finished(server));
         ++i) {
        (void)SSL_do_handshake(client);
        transfer(client, server);
        (void)SSL_do_handshake(server);
        transfer(server, client);
    }
    assert(SSL_is_init_finished(client) && SSL_is_init_finished(server));
}

static void split_records(SSL *client, SSL *server) {
    NiceSocket base = {0}, socket = {0};
    TlsSocket tls = {0};
    char first[4] = {0}, second[5] = {0};
    GInputVector vectors[] = {{first, sizeof(first)}, {second, sizeof(second)}};
    NiceInputMessage message = {vectors, 2, NULL, 0};
    tls.base = &base;
    tls.ssl = client;
    tls.ready = TRUE;
    socket.priv = &tls;
    assert(SSL_write(server, "ab", 2) == 2);
    assert(SSL_write(server, "cde", 3) == 3);
    assert(SSL_write(server, "fghi", 4) == 4);
    transfer(server, client);
    assert(read_message(&socket, &message) == 1);
    assert(message.length == 9 && memcmp(first, "abcd", 4) == 0 && memcmp(second, "efghi", 5) == 0);
    assert(read_message(&socket, &message) == 0);
}

static void partial_record(SSL *client, SSL *server) {
    NiceSocket base = {0}, socket = {0};
    TlsSocket tls = {0};
    char data[5] = {0};
    GInputVector vector = {data, sizeof(data)};
    NiceInputMessage message = {&vector, 1, NULL, 0};
    tls.base = &base;
    tls.ssl = client;
    tls.ready = TRUE;
    socket.priv = &tls;
    assert(SSL_write(server, "xyz", 3) == 3);
    transfer(server, client);
    assert(read_message(&socket, &message) == 1);
    assert(message.length == 3 && memcmp(data, "xyz", 3) == 0);
    assert(read_message(&socket, &message) == 0);
}

static void close_after_data(SSL_CTX *client_context, SSL_CTX *server_context, gboolean alert) {
    SSL *client = make_ssl(client_context, FALSE);
    SSL *server = make_ssl(server_context, TRUE);
    NiceSocket base = {0}, socket = {0};
    TlsSocket tls = {0};
    char data[5] = {0}, untouched[5] = {0};
    GInputVector vectors[] = {{data, sizeof(data)}, {untouched, sizeof(untouched)}};
    NiceInputMessage messages[] = {{vectors, 1, NULL, 0}, {vectors + 1, 1, NULL, 42}};
    do_handshake(client, server);
    tls.base = &base;
    tls.ssl = client;
    tls.ready = TRUE;
    socket.priv = &tls;
    assert(SSL_write(server, "xyz", 3) == 3);
    if (alert)
        assert(SSL_shutdown(server) == 0);
    transfer(server, client);
    fake_eof = !alert;
    fake_reads = 0;
    assert(tls_recv(&socket, messages, 2, NULL) == 1);
    assert(messages[0].length == 3 && memcmp(data, "xyz", 3) == 0);
    assert(messages[1].length == 42 && untouched[0] == 0);
    assert(fake_reads == (alert ? 0U : 1U));
    assert(tls_recv(&socket, messages, 2, NULL) == -1);
    fake_eof = FALSE;
    SSL_free(client);
    SSL_free(server);
}

static void flush_failure_after_data(SSL_CTX *client_context, SSL_CTX *server_context) {
    SSL *client = make_ssl(client_context, FALSE), *server = make_ssl(server_context, TRUE);
    NiceSocket base = {0}, socket = {0};
    TlsSocket tls = {0};
    char data[5] = {0};
    GInputVector vector = {data, sizeof(data)};
    NiceInputMessage message = {&vector, 1, NULL, 0};
    do_handshake(client, server);
    tls.base = &base;
    tls.ssl = client;
    tls.ready = TRUE;
    socket.priv = &tls;
    assert(SSL_write(server, "xyz", 3) == 3);
    transfer(server, client);
    assert(BIO_write(SSL_get_wbio(client), "x", 1) == 1);
    fake_send_failure = TRUE;
    assert(tls_recv(&socket, &message, 1, NULL) == 1);
    assert(message.length == 3 && memcmp(data, "xyz", 3) == 0);
    assert(tls_recv(&socket, &message, 1, NULL) == -1);
    fake_send_failure = FALSE;
    SSL_free(client);
    SSL_free(server);
}

static void queue_admission(void) {
    NiceSocket socket = {0};
    TlsSocket tls = {0};
    char small = 0;
    GOutputVector vectors[] = {{&small, 1}, {&small, TLS_QUEUE_LIMIT}};
    NiceOutputMessage messages[] = {{vectors, 1}, {vectors + 1, 1}};
    socket.priv = &tls;
    assert(tls_send_reliable(&socket, NULL, messages, 2) == -1);
    assert(g_queue_is_empty(&tls.pending) && tls.pending_bytes == 0);
    assert(tls_send_reliable(&socket, NULL, messages, 1) == 1);
    assert(g_queue_get_length(&tls.pending) == 1 && tls.pending_bytes == 1);
    NiceOutputMessage many[TLS_QUEUE_RECORDS];
    for (unsigned i = 0; i < TLS_QUEUE_RECORDS; ++i)
        many[i] = messages[0];
    assert(tls_send_reliable(&socket, NULL, many, TLS_QUEUE_RECORDS) == -1);
    assert(g_queue_get_length(&tls.pending) == 1 && tls.pending_bytes == 1);
    nice_socket_free_send_queue(&tls.pending);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    SSL_CTX *client_context = SSL_CTX_new(TLS_client_method());
    SSL_CTX *server_context = SSL_CTX_new(TLS_server_method());
    assert(client_context && server_context);
    assert(SSL_CTX_use_certificate_file(server_context, argv[1], SSL_FILETYPE_PEM) == 1);
    assert(SSL_CTX_use_PrivateKey_file(server_context, argv[2], SSL_FILETYPE_PEM) == 1);
    SSL *client = make_ssl(client_context, FALSE);
    SSL *server = make_ssl(server_context, TRUE);
    do_handshake(client, server);
    split_records(client, server);
    partial_record(client, server);
    close_after_data(client_context, server_context, TRUE);
    close_after_data(client_context, server_context, FALSE);
    flush_failure_after_data(client_context, server_context);
    queue_admission();
    SSL_free(client);
    SSL_free(server);
    SSL_CTX_free(client_context);
    SSL_CTX_free(server_context);
    return 0;
}
