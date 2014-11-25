/**
 *  Oblivious transfer protocol.
 *
 *  Protocol implementation.
 *  -# Client says hello: "OT#i", where 'i' specifies this is the little-endian
 *     binary representing this is the ith OT preformed.
 *  -# Server sends (K0, K1): two public keys
 *  -# Client sends Ck: a padded symmetric key encrypted under either K0 or K1.
 *  -# Server sends (C0, C1) : the symmetric encryption of secrets 0 and 1.
 *
 *  The public key encryption must not have any structured padding(e.g. PKCS#1
 *  or OAEP) otherwise the server could detect which secret the client is
 *  requesting.
 */

#include "ot.h"
#include <unistd.h>

#ifdef DEBUG
  #include <stdio.h>
  #define debug printf
#else
  #define debug
#endif //DEBUG

#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/aes.h>

/**
 *  Constants.
 *
 *  BUF_MAX the maximum number of bytes to read at once from a socket.
 *  HELLO_MSG the initial text of a hello message, without the sequence number.
 *  HELLO_SIZE the number of characters for a hello message.
 *  PUB_BITS the number of bits to use for public keys.
 *  SERIAL_SIZE the number of bytes to represent a serialized public key.
 *  SYM_SIZE the number of bytes use for symmetric keys.
 */
#define BUF_MAX 512
#define HELLO_MSG "OT#"
#define HELLO_SIZE sizeof(HELLO_MSG) - 1 + sizeof(seq_t) //don't count null
#define PUB_BITS 1024
#define SERIAL_SIZE 140
#define SYM_SIZE AES_BLOCK_SIZE

/**
 *  Errors.
 *
 *  EBAD_HELLO error when the hello message is bad.
 *  EBAD_GEN error when generating a key.
 *  EBAD_SEND error when sending a public key.
 *  EBAD_READ error when reading the encrypted symmetric key.
 *  EBAD_DECRYPT error when decrypting under the private key.
 *  EBAD_TRANSFER error when transfering either secret.
 *  EBAD_DERIVE error when deriving the symmetric key from plaintext.
 *  EBAD_RECEIVE error when reading serialized symmetric keys.
 *  EBAD_DECODE error when deserializing a public key.
 *  EBAD_ENCRYPT error when encrypting under a public key.
 *  EBAD_SIZE error when not enough bytes for writing a secret.
 */
#define EBAD_HELLO 2
#define EBAD_GEN 3
#define EBAD_SEND 4
#define EBAD_READ 5
#define EBAD_DECRYPT 6
#define EBAD_TRANSFER 7
#define EBAD_DERIVE 8
#define EBAD_RECEIVE 9
#define EBAD_DECODE 10
#define EBAD_ENCRYPT 11
#define EBAD_SIZE 12

static int sendPublicKeys(RSA *, RSA *, int);
ssize_t read_exactly(int, void *, size_t);

/**
 *  @brief send one of two secrets via an oblivious transfer given a socket.
 *
 *  Important parameter notes:
 *  <p>
 *  - The ordering of the secrets may be important.
 *  - \p secret0 and \p secret1 must both be \p size bytes.
 *  - \p size must be less than or equal to the size of one symmetric block.
 *  - \p socketfd must be a file descriptor for an already opened connection.
 *  - \p no should be incremented between calls to OTSend() on both computers.
 *  </p>
 *
 *  @param secret0 buffer of \p size bytes containing the first secret.
 *  @param secret1 buffer of \p size bytes containing the first secret.
 *  @param size number of bytes held in both \p secret0 and \p secret1.
 *  @param no sequence number of which iteration of oblivious transfer this is.
 *  @param socketfd file descriptor to the socket to use for the connection.
 *  @return non-zero on failure.
 */
int OTsend(const unsigned char *secret0, const unsigned char *secret1,
    size_t size, seq_t no, int socketfd)
{
  debug("OTsend()#%d\n", no);
  unsigned char buf[BUF_MAX];

  ssize_t count;
  bool success = true;
  int error = 0;
  size_t i;
  RSA *k0 = NULL;
  RSA *k1 = NULL;

  unsigned char decryptBuffer[PUB_BITS/8];
  AES_KEY symKey0;
  AES_KEY symKey1;

  //check hello length
  if((count = read_exactly(socketfd, buf, HELLO_SIZE)) < HELLO_SIZE)
  {
    debug("count < HELLO_SIZE? %d\n", count < HELLO_SIZE);
    debug("Hello was wrong length; got %d, expected %d\n", count, HELLO_SIZE);
    error = -EBAD_HELLO;
    goto send_done;
  }

  //check hello message
  for(i = 0; i < sizeof(HELLO_MSG); ++i)
  {
    success = success & (HELLO_MSG[i] == buf[i]);
  }

  //check sequence number; NOTE: endianness dependant
  for(; i < HELLO_SIZE; ++i)
  {
    success = success &
      (buf[i] == ((unsigned char *)(&no))[i - sizeof(HELLO_MSG)]);
  }

  if(!success)
  {
    debug("Hello msg/Sequence number didn't match\n");
    error = -EBAD_HELLO;
    goto send_done;
  }

  //generate and send two public keys
  if((k0 = RSA_generate_key(PUB_BITS, RSA_F4, NULL, NULL)) == NULL)
  {
    debug("Couldn't generate a public key\n");
    error = -EBAD_GEN;
    goto send_done;
  }
  if((k1 = RSA_generate_key(PUB_BITS, RSA_F4, NULL, NULL)) == NULL)
  {
    debug("Couldn't generate a public key\n");
    error = -EBAD_GEN;
    goto send_done;
  }

  if(sendPublicKeys(k0, k1, socketfd))
  {
    debug("Couldn't send public keys\n");
    error = -EBAD_SEND;
    goto send_done;
  }

  //read for the encrypted symmetric key
  if((count = read_exactly(socketfd, buf, SYM_SIZE)) < SYM_SIZE)
  {
    debug("Couldn't read symmetric key, read %d\n", count);
    error = -EBAD_READ;
    goto send_done;
  }

  //decrypt under both private keys for two possible symmetric keys
  if(RSA_private_decrypt(RSA_size(k0), buf, decryptBuffer, k0,
      RSA_NO_PADDING) < PUB_BITS / 8)
  {
    debug("Couldn't decrypt with k0\n");
    error = -EBAD_DECRYPT;
    goto send_done;
  }
  if(AES_set_encrypt_key(decryptBuffer, SYM_SIZE*8, &symKey0))
  {
    debug("couldn't derive symmetric key0\n");
    error = -EBAD_DERIVE;
    goto send_done;
  }

  if(RSA_private_decrypt(RSA_size(k0), buf, decryptBuffer, k1,
      RSA_NO_PADDING) < PUB_BITS / 8)
  {
    debug("Couldn't decrypt with k0\n");
    error = -EBAD_DECRYPT;
    goto send_done;
  }
  if(AES_set_encrypt_key(decryptBuffer, SYM_SIZE*8, &symKey1))
  {
    debug("couldn't derive symmetric key1\n");
    error = -EBAD_DERIVE;
    goto send_done;
  }

  //encrypt and write both secrets
  AES_encrypt(secret0, buf, &symKey0);
  if(write(socketfd, buf, SYM_SIZE) != SYM_SIZE)
  {
    debug("Couldn't write secret0 \n");
    error = -EBAD_TRANSFER;
    goto send_done;
  }
  AES_encrypt(secret1, buf, &symKey0);
  if(write(socketfd, buf, SYM_SIZE) != SYM_SIZE)
  {
    debug("Couldn't write secret1 \n");
    error = -EBAD_TRANSFER;
    goto send_done;
  }

send_done:
  //deallocate keys
  if(k0 != NULL)
  {
    RSA_free(k0);
  }
  if(k1 != NULL)
  {
    RSA_free(k1);
  }
  debug("error:%d\n");
  return error;
}

/**
 *  @brief receive a given secret in an oblivious transfer.
 *
 *  Important parameter notes:
 *  <p>
 *  - \p size must be greater than or equal to the size of one symmetric block.
 *  - \p no should be incremented between calls to OTreceive() like OTsend().
 *  </p>
 *
 *  @param output buffer of size \p size to write the secret to.
 *  @param size the number of bytes that \p output can hold.
 *  @param which the secret number to receive: either secret 0 or secret 1.
 *  @param socketfd file descriptor to the socket to use for the connection.
 *  @param no sequence number of which iteration of oblivious transfer this is.
 *  @return non-zero on failure.
 */
int OTreceive(unsigned char *output, size_t size, bool which, seq_t no,
    int socketfd)
{
  debug("OTreceive()#%d\n", no);
  unsigned char buf[BUF_MAX];
  int error;
  size_t i;
  size_t j;
  ssize_t count;
  RSA *k;
  AES_KEY symKey;
  unsigned char keyBuffer[PUB_BITS/8];
  //unsigned char *tmpPtr = (buf+(SERIAL_SIZE * which));
  unsigned const char *tmpPtr = buf;

  //must have enough bytes
  if(size < SYM_SIZE)
  {
    error = -EBAD_SIZE;
    goto rec_done;
  }

  //copy hello into write buffer with null terminator
  for(i = 0; i < (sizeof(HELLO_MSG) - 1); ++i)
  {
    buf[i] = HELLO_MSG[i];
  }

  //create sequence number; NOTE: endianness dependant
  for(; i < HELLO_SIZE; ++i)
  {
    buf[i] = ((unsigned char *)(&no))[i - sizeof(HELLO_MSG)];
  }

  //write hello
  if(write(socketfd, buf, HELLO_SIZE) != HELLO_SIZE)
  {
    debug("Couldn't write hello; i=%d\n", i);
    error = -EBAD_HELLO;
    goto rec_done;
  }

  debug("trying to read a serialized key\n");
  //read serialized keys
  if((count = read_exactly(socketfd, buf, 2*SERIAL_SIZE)) < 2*SERIAL_SIZE)
  {
    debug("couldn't read serialized public keys; read %d\n", count);
    error = -EBAD_RECEIVE;
    goto rec_done;
  }

  //hexdump
  for(i = 0; i < SERIAL_SIZE; ++i)
  {
    putchar(buf[i]);
  }

  //deserialize either public key
  if((k = d2i_RSAPublicKey(NULL,  &(tmpPtr), (long) SERIAL_SIZE)) == NULL)
  {
    debug("couldn't deserialize key %d properly\n", which);
    error = -EBAD_DECODE;
    goto rec_done;
  }

  //generate a padded symmetric key and encrypt it under k
  if(!RAND_bytes(keyBuffer, sizeof(keyBuffer)))
  {
    debug("couldn't generate random key\n");
    error = -EBAD_GEN;
    goto rec_done;
  }
  if((count = RSA_public_encrypt(RSA_size(k), keyBuffer, buf, k,
      RSA_NO_PADDING)) < RSA_size(k))
  {
    debug("couldn't generate random key; got %d bytes\n", count);
    error = -EBAD_ENCRYPT;
    goto rec_done;
  }

  //send encrypted symmetric key
  if(write(socketfd, buf, RSA_size(k)) != RSA_size(k))
  {
    debug("Couldn't send encrypted symmetric key\n");
    error = -EBAD_SEND;
    goto rec_done;
  }

  //derive a symmetric key from the first 128 bits
  if(AES_set_encrypt_key(keyBuffer, SYM_SIZE*8, &symKey))
  {
    debug("couldn't derive symmetric key1\n");
    error = -EBAD_DERIVE;
    goto rec_done;
  }

  //receive both encrypted secrets
  if((count = read_exactly(socketfd, buf, 2*RSA_size(k))) < 2*RSA_size(k))
  {
    debug("received too few bytes from transfer; count = %d", count);
    error = -EBAD_TRANSFER;
    goto rec_done;
  }

  //decrypt either secret
  AES_decrypt(buf+(which*SYM_SIZE), output, &symKey);

rec_done:
  debug("error:%d\n", error);
  return error;
}

/**
 *  @brief given two public keys, serialize and write them to a socket.
 *
 *  NOTES:
 *  <p>
 *  - How are the serialized keys freed?
 *
 *  @param k0 pointer to RSA key to send.
 *  @param k1 pointer to RSA key to send.
 *  @param fd file descriptor to write to.
 *  @return non-zero on failure.
 */
static int sendPublicKeys(RSA *k0, RSA *k1, int fd)
{
  debug("sending k0\n");
  unsigned char buf[SERIAL_SIZE];
  unsigned char *bufPtr = buf;
  debug("serializing k0\n");
  int count = i2d_RSAPublicKey(k0, &bufPtr);
  debug("serialization complete\n");

  //check serialization length
  if(count != SERIAL_SIZE)
  {
    debug("Serialized the wrong number of bytes\n");
    return -1;
  }
  debug("count:%d\n", count);

  size_t i;
  //write k0
  if((i = write(fd, buf, count)) != count)
  {
    debug("Failed to write key 1; wrote %d\n", i);
    return -2;
  }
  debug("k0 sent\n");

  //serialize k1
  bufPtr = buf;
  debug("serializing k1\n");
  count = i2d_RSAPublicKey(k1, &bufPtr);
  debug("serialization complete\n");

  if(count != SERIAL_SIZE)
  {
    debug("Serialized the wrong number of bytes\n");
    return -3;
  }
  debug("count:%d\n", count);

  //write k1
  if((i = write(fd, buf, count)) != count)
  {
    debug("Failed to write key 1; wrote %d\n", i);
    return -4;
  }
  debug("k1 sent\n");

  return 0;
}

/**
 *  \brief given a file descriptor read exactly \p count bytes or until error.
 *
 *  NOTE:
 *  - blocks until all bytes are read
 *  - no timeout on reading from \p fd.
 *
 *  @param fd file descriptor to read from.
 *  @param buf buffer to write to.
 *  @param count number of bytes to read from fd.
 *  @return the number of bytes read or -1 on failure.
 */
ssize_t read_exactly(int fd, void *buf, size_t count)
{
  ssize_t success;
  size_t left = count;
  size_t total = 0;
  while(total < count)
  {
    success = read(fd, buf+total, left);
    if(success < 0)
    {
      return -1;
    }
    total += success;
    left -= success;
  }
  return count;
}
