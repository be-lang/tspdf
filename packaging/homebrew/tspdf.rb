class Tspdf < Formula
  desc "Zero-dependency PDF toolkit and CLI in pure C11"
  homepage "https://github.com/be-lang/tspdf"
  url "https://github.com/be-lang/tspdf/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "a291e593aad65e96cd743afd07531edf68862df13c9ed0f48779d5d0552efda5"
  license "MIT"

  def install
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system bin/"tspdf", "qrcode", "https://example.org", "-o", testpath/"qr.pdf"
    assert_equal "%PDF-", (testpath/"qr.pdf").binread(5)
  end
end
