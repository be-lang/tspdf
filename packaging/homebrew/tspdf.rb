class Tspdf < Formula
  desc "Zero-dependency PDF toolkit and CLI in pure C11"
  homepage "https://github.com/be-lang/tspdf"
  url "https://github.com/be-lang/tspdf/archive/refs/tags/v0.3.0.tar.gz"
  sha256 "1545411ededcfb8cd517e040bdf5a2d2da135a41b19c7104c112078a3c4acfec"
  license "MIT"

  def install
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    system bin/"tspdf", "qrcode", "https://example.org", "-o", testpath/"qr.pdf"
    assert_equal "%PDF-", (testpath/"qr.pdf").binread(5)
  end
end
