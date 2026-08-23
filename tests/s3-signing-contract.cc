#include <shared/services/storage/s3-signing.hxx>

#include <cassert>
#include <string>

int main()
{
  using namespace s3_signing;

  SigV4Input input;
  input.method = "GET";
  input.canonicalUri = "/test.txt";
  input.payloadHash = sha256Hex("");
  input.accessKey = "AKIAIOSFODNN7EXAMPLE";
  input.secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
  input.region = "us-east-1";
  input.timestamp = "20130524T000000Z";
  input.headers = {
      {"host", "examplebucket.s3.amazonaws.com"},
      {"range", "bytes=0-9"},
      {"x-amz-content-sha256", input.payloadHash},
      {"x-amz-date", input.timestamp},
  };

  const auto signedRequest = sign(input);
  assert(input.payloadHash ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  assert(signedRequest.signature ==
         "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
  assert(signedRequest.authorization ==
         "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/"
         "s3/aws4_request,SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,"
         "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}
