#include "DNSServer.h"
#include <lwip/def.h>

#include <cctype>
#include <cstring>

#define DNS_MIN_REQ_LEN 17  // minimal size for DNS request asking ROOT = DNS_HEADER_SIZE + 1 null byte for Name + 4 bytes type/class

DNSServer::DNSServer() : _port(DNS_DEFAULT_PORT), _ttl(htonl(DNS_DEFAULT_TTL)), _errorReplyCode(DNSReplyCode::NonExistentDomain) {}

DNSServer::DNSServer(const char *domainName)
  : _port(DNS_DEFAULT_PORT), _ttl(htonl(DNS_DEFAULT_TTL)), _errorReplyCode(DNSReplyCode::NonExistentDomain) {
  if (domainName && (std::strlen(domainName) < sizeof(_domainName))) {
    std::strcpy(_domainName, domainName);
  }
}

bool DNSServer::start() {
  if (!_resolvedIP.value()) {
    return false;  // no address is set, an overloaded method must be used to get device's IP
                   // start(uint16_t port, const char *domainName, espurna::Ip4 resolvedIP)
  }

  _udp.close();
  _udp.onPacket([this](espurna::udp::Packet &pkt) {
    this->_handleUDP(pkt);
  });
  return _udp.listen(_resolvedIP.value(), _port);
}

bool DNSServer::start(uint16_t port, const char *domainName, espurna::Ip4 resolvedIP) {
  _port = port;
  _domainName[0] = 0;
  if (domainName && (std::strcmp(domainName, "*") != 0)) {
    if (std::strlen(domainName) >= sizeof(_domainName)) {
      return false;
    }
    std::strcpy(_domainName, domainName);
    downcaseAndRemoveWwwPrefix(_domainName);
  }

  _resolvedIP = resolvedIP;
  _udp.close();
  _udp.onPacket([this](espurna::udp::Packet &pkt) {
    this->_handleUDP(pkt);
  });
  // Bound to the AP address, not to any: the Beken SDK DHCP server keeps its own catch-all socket on port 53 that
  // refuses every query, lwIP prefers the socket bound to the address. Other chips answer on the AP only as well
  return _udp.listen(_resolvedIP.value(), _port);
}

void DNSServer::setErrorReplyCode(const DNSReplyCode &replyCode) {
  _errorReplyCode = replyCode;
}

void DNSServer::setTTL(const uint32_t &ttl) {
  _ttl = htonl(ttl);
}

void DNSServer::stop() {
  _udp.close();
}

// lower case, every "www." taken out (as String::replace("www.", "") of the original)
void DNSServer::downcaseAndRemoveWwwPrefix(char *domainName) {
  char *out = domainName;
  for (const char *in = domainName; *in;) {
    if (std::strncmp(in, "www.", 4) == 0) {
      in += 4;
      continue;
    }
    *out++ = static_cast<char>(std::tolower(static_cast<unsigned char>(*in++)));
  }
  *out = 0;
}

void DNSServer::_handleUDP(espurna::udp::Packet &pkt) {
  if (pkt.length() < DNS_MIN_REQ_LEN) {
    return;  // truncated packet or not a DNS req
  }

  // get DNS header (beginning of message)
  DNSHeader dnsHeader;
  DNSQuestion dnsQuestion;
  memcpy(&dnsHeader, pkt.data(), DNS_HEADER_SIZE);
  if (dnsHeader.QR != DNS_QR_QUERY) {
    return;  // ignore non-query messages
  }

  if (requestIncludesOnlyOneQuestion(dnsHeader)) {
    /*
      // The QName has a variable length, maximum 255 bytes and is comprised of multiple labels.
      // Each label contains a byte to describe its length and the label itself. The list of
      // labels terminates with a zero-valued byte. In "github.com", we have two labels "github" & "com"
*/
    // the end of label marker, looked for inside the packet only: a packet without one is never read past its end
    const char *enoflbls = static_cast<const char *>(
      memchr(reinterpret_cast<const char *>(pkt.data()) + DNS_HEADER_SIZE, 0, pkt.length() - DNS_HEADER_SIZE));
    if (!enoflbls) {
      return;  // malformed packet
    }
    ++enoflbls;  // advance after null terminator
    dnsQuestion.QName = pkt.data() + DNS_HEADER_SIZE;                                                // we can reference labels from the request
    dnsQuestion.QNameLength = enoflbls - (char *)pkt.data() - DNS_HEADER_SIZE;
    /*
        check if we aint going out of pkt bounds
        proper dns req should have label terminator at least 4 bytes before end of packet
      */
    if (dnsQuestion.QNameLength > pkt.length() - DNS_HEADER_SIZE - sizeof(dnsQuestion.QType) - sizeof(dnsQuestion.QClass)) {
      return;  // malformed packet
    }

    // Copy the QType and QClass
    memcpy(&dnsQuestion.QType, enoflbls, sizeof(dnsQuestion.QType));
    memcpy(&dnsQuestion.QClass, enoflbls + sizeof(dnsQuestion.QType), sizeof(dnsQuestion.QClass));
  }

  // will reply with IP only to "*" or if domain matches without www. subdomain
  char name[DNS_NAME_MAX];
  if (dnsHeader.OPCode == DNS_OPCODE_QUERY && requestIncludesOnlyOneQuestion(dnsHeader)
      && (isCaptive()
          || (getDomainNameWithoutWwwPrefix(static_cast<const unsigned char *>(dnsQuestion.QName), dnsQuestion.QNameLength,
                name, sizeof(name))
              && (std::strcmp(name, _domainName) == 0)))) {

    // Qtype = A (1) or ANY (255): send an A record otherwise an empty response
    if (ntohs(dnsQuestion.QType) == 1 || ntohs(dnsQuestion.QType) == 255) {
      replyWithIP(pkt, dnsHeader, dnsQuestion);
    } else {
      replyWithNoAnsw(pkt, dnsHeader, dnsQuestion);
    }
    return;
  }
  // otherwise reply with custom code
  replyWithCustomCode(pkt, dnsHeader);
}

bool DNSServer::requestIncludesOnlyOneQuestion(DNSHeader &dnsHeader) {
  dnsHeader.ARCount = 0;  // We assume that if ARCount !=0 there is a EDNS OPT packet, just ignore
  return ntohs(dnsHeader.QDCount) == 1 && dnsHeader.ANCount == 0 && dnsHeader.NSCount == 0;
}

bool DNSServer::getDomainNameWithoutWwwPrefix(const unsigned char *start, size_t len, char *out, size_t size) {
  // the labels without the trailing null byte: "<n>label<n>label" -> ".label.label" -> "label.label"
  if (!len || (--len >= size)) {
    return false;
  }

  std::memcpy(out, start, len);
  out[len] = 0;

  size_t pos = 0;
  while (pos < len) {
    const size_t label = static_cast<unsigned char>(out[pos]);
    out[pos] = '.';
    pos += label + 1;
  }

  std::memmove(out, out + 1, len);  // the first "." out, the terminator moves with it
  downcaseAndRemoveWwwPrefix(out);
  return true;
}

void DNSServer::replyWithIP(espurna::udp::Packet &req, DNSHeader &dnsHeader, DNSQuestion &dnsQuestion) {
  espurna::udp::Message rpl;
  // Change the type of message to a response and set the number of answers equal to
  // the number of questions in the header
  dnsHeader.QR = DNS_QR_RESPONSE;
  dnsHeader.ANCount = dnsHeader.QDCount;
  rpl.write((unsigned char *)&dnsHeader, DNS_HEADER_SIZE);

  // Write the question
  rpl.write(dnsQuestion.QName, dnsQuestion.QNameLength);
  rpl.write((uint8_t *)&dnsQuestion.QType, 2);
  rpl.write((uint8_t *)&dnsQuestion.QClass, 2);

  // Write the answer
  // Use DNS name compression : instead of repeating the name in this RNAME occurrence,
  // set the two MSB of the byte corresponding normally to the length to 1. The following
  // 14 bits must be used to specify the offset of the domain name in the message
  // (<255 here so the first byte has the 6 LSB at 0)
  rpl.write((uint8_t)0xC0);
  rpl.write((uint8_t)DNS_OFFSET_DOMAIN_NAME);

  // DNS type A : host address, DNS class IN for INternet, returning an IPv4 address
  uint16_t answerType = htons(DNS_TYPE_A), answerClass = htons(DNS_CLASS_IN), answerIPv4 = htons(DNS_RDLENGTH_IPV4);
  rpl.write((unsigned char *)&answerType, 2);
  rpl.write((unsigned char *)&answerClass, 2);
  rpl.write((unsigned char *)&_ttl, 4);  // DNS Time To Live
  rpl.write((unsigned char *)&answerIPv4, 2);
  uint32_t ip = _resolvedIP.value();
  rpl.write(reinterpret_cast<uint8_t *>(&ip), sizeof(uint32_t));  // The IPv4 address to return

  _udp.sendTo(rpl, req.remoteIP(), req.remotePort());
}

void DNSServer::replyWithCustomCode(espurna::udp::Packet &req, DNSHeader &dnsHeader) {
  dnsHeader.QR = DNS_QR_RESPONSE;
  dnsHeader.RCode = static_cast<uint16_t>(_errorReplyCode);
  dnsHeader.QDCount = 0;

  espurna::udp::Message rpl(sizeof(DNSHeader));
  rpl.write(reinterpret_cast<const uint8_t *>(&dnsHeader), sizeof(DNSHeader));
  _udp.sendTo(rpl, req.remoteIP(), req.remotePort());
}

void DNSServer::replyWithNoAnsw(espurna::udp::Packet &req, DNSHeader &dnsHeader, DNSQuestion &dnsQuestion) {

  dnsHeader.QR = DNS_QR_RESPONSE;
  dnsHeader.ANCount = 0;
  dnsHeader.NSCount = htons(1);

  espurna::udp::Message rpl;
  rpl.write(reinterpret_cast<const uint8_t *>(&dnsHeader), sizeof(DNSHeader));

  // Write the question
  rpl.write(dnsQuestion.QName, dnsQuestion.QNameLength);
  rpl.write((uint8_t *)&dnsQuestion.QType, 2);
  rpl.write((uint8_t *)&dnsQuestion.QClass, 2);

  // An empty answer contains an authority section with a SOA,
  // We take the name of the query as the root of the zone for which the SOA is generated
  // and use a value of DNS_MINIMAL_TTL seconds  in order to minimize negative caching
  // Write the authority section:
  // The SOA RR's ownername is set equal to the query name, and we use made up names for
  // the MNAME and  RNAME - it doesn't really matter from a protocol perspective - as for
  // a no such QTYPE answer only the timing fields are used.
  // a protocol perspective - it
  // Use DNS name compression : instead of repeating the name in this RNAME occurrence,
  // set the two MSB of the byte corresponding normally to the length to 1. The following
  // 14 bits must be used to specify the offset of the domain name in the message
  // (<255 here so the first byte has the 6 LSB at 0)
  rpl.write((uint8_t)0xC0);
  rpl.write((uint8_t)DNS_OFFSET_DOMAIN_NAME);

  // DNS type A : host address, DNS class IN for INternet, returning an IPv4 address
  uint16_t answerType = htons(DNS_TYPE_SOA), answerClass = htons(DNS_CLASS_IN);
  uint32_t Serial = htonl(DNS_SOA_SERIAL);    // Date type serial based on the date this piece of code was written
  uint32_t Refresh = htonl(DNS_SOA_REFRESH);  // These timers don't matter, we don't serve zone transfers
  uint32_t Retry = htonl(DNS_SOA_RETRY);
  uint32_t Expire = htonl(DNS_SOA_EXPIRE);
  uint32_t MinTTL = htonl(DNS_MINIMAL_TTL);  // See RFC2308 section 5
  char MLabel[] = DNS_SOA_MNAME_LABEL;
  char RLabel[] = DNS_SOA_RNAME_LABEL;
  char PostFixLabel[] = DNS_SOA_POSTFIX_LABEL;

  // 4 accounts for len fields and  for both rname
  // and lname and their postfix labels and there are 5 32 bit fields

  uint16_t RdataLength = htons((uint16_t)(strlen(MLabel) + strlen(RLabel) + 2 * strlen(PostFixLabel) + 4 + 5 * sizeof(Serial)));

  rpl.write((unsigned char *)&answerType, 2);
  rpl.write((unsigned char *)&answerClass, 2);
  rpl.write((unsigned char *)&MinTTL, 4);  // DNS Time To Live

  rpl.write((unsigned char *)&RdataLength, 2);

  rpl.write((uint8_t)strlen(MLabel));
  rpl.write((unsigned char *)&MLabel, strlen(MLabel));

  rpl.write((unsigned char *)&PostFixLabel, strlen(PostFixLabel));
  rpl.write((uint8_t)0);
  //  rpl.write((uint8_t)0xC0);
  //  rpl.write((uint8_t)DNS_OFFSET_DOMAIN_NAME);

  rpl.write((uint8_t)strlen(RLabel));
  rpl.write((unsigned char *)&RLabel, strlen(RLabel));
  rpl.write((unsigned char *)&PostFixLabel, strlen(PostFixLabel));
  rpl.write((uint8_t)0);

  rpl.write((unsigned char *)&Serial, 4);
  rpl.write((unsigned char *)&Refresh, 4);
  rpl.write((unsigned char *)&Retry, 4);
  rpl.write((unsigned char *)&Expire, 4);
  rpl.write((unsigned char *)&MinTTL, 4);

  _udp.sendTo(rpl, req.remoteIP(), req.remotePort());
}
