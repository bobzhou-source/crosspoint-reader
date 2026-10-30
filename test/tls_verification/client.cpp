#include <SecureHttpClient.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
  assert(argc == 6);
  const uint16_t goodPort = std::stoi(argv[1]);
  const uint16_t expiredPort = std::stoi(argv[2]);
  const uint16_t mismatchPort = std::stoi(argv[3]);
  const uint16_t plainPort = std::stoi(argv[4]);
  std::ifstream file(argv[5]);
  const std::string root((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  assert(!root.empty());
  freeink::SecureClient client;
  client.setTimeout(3000);
  // The default public store must NOT trust an arbitrary local test CA.
  assert(!client.connect("localhost", goodPort));
  client.setCACert("not a PEM certificate");
  assert(!client.connect("localhost", goodPort));
  client.setCACert(root.c_str());
  assert(client.connect("localhost", goodPort));
  client.stop();
  assert(!client.connect("wrong.example", goodPort));
  assert(!client.connect("localhost", expiredPort));
  // SAN takes precedence even though this certificate's CN is localhost.
  assert(!client.connect("localhost", mismatchPort));
  assert(client.connect("localhost", goodPort));
  client.setCACert(nullptr);
  assert(!client.connected());
  assert(!client.connect("localhost", goodPort));

  freeink::SecureHttpClient http;
  http.setCACert(root.c_str());
  http.setFollowRedirects(5);
  const std::string https = "https://localhost:" + std::to_string(goodPort);
  assert(http.begin(https + "/good"));
  assert(http.GET() == 200 && http.getString() == "verified");
  assert(http.begin(https + "/downgrade"));
  assert(http.GET() == 302);  // A second request over plaintext is forbidden.
  http.end();
  assert(http.begin("http://localhost:" + std::to_string(plainPort) + "/local"));
  assert(http.GET() == 200 && http.getString() == "local");
  http.end();
  std::string url = "HTTPS://localhost/a";
  assert(!freeink::SecureHttpClient::resolveUrl(url, "hTtP://localhost/b", url));
  assert(url == "HTTPS://localhost/a");
  assert(freeink::SecureHttpClient::resolveUrl(url, "//other.example/b", url));
  assert(url == "https://other.example/b");
  assert(freeink::SecureHttpClient::resolveUrl(url, "/c", url));
  assert(url == "https://other.example/c");
  assert(freeink::SecureHttpClient::resolveUrl("http://localhost/a", "/local", url));
  assert(url == "http://localhost/local");
  std::puts("PASS: real wolfSSL valid chain+host; untrusted, malformed CA, expired, wrong-host/SAN rejection; trust reset; no downgrade; local HTTP");
}
