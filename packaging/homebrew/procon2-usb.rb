# Homebrew formula for procon2-usb. Copy into the MikeFE/homebrew-tap repo as
# Formula/procon2-usb.rb, then fill in `sha256` for the tagged release:
#   curl -sL https://github.com/MikeFE/procon2-usb/archive/refs/tags/v0.1.0.tar.gz | shasum -a 256
class Procon2Usb < Formula
  desc "Makes a wired Switch 2 Pro Controller work as a gamepad on macOS"
  homepage "https://github.com/MikeFE/procon2-usb"
  url "https://github.com/MikeFE/procon2-usb/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "REPLACE_WITH_RELEASE_TARBALL_SHA256"

  depends_on macos: :ventura

  def install
    # Native build; the Makefile defaults to a universal binary
    system "make", "ARCHS="
    bin.install "build/procon2-usb"
    pkgshare.install "sdl/gamecontrollerdb.txt"
  end

  service do
    run [opt_bin/"procon2-usb", "agent", "--sdl-mapping", opt_pkgshare/"gamecontrollerdb.txt"]
    keep_alive successful_exit: false
    process_type :interactive
  end

  def caveats
    <<~EOS
      Start the background helper (it also starts at every login):
        brew services start procon2-usb

      Then quit and reopen Moonlight once.
    EOS
  end

  test do
    assert_match "set report format", shell_output("#{bin}/procon2-usb sequence")
  end
end
