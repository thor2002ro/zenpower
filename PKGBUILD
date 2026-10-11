# Build the sources in this checkout; no upstream download is required.
pkgname=zenpower-dkms
pkgver=0.6.0
pkgrel=1
pkgdesc='AMD Zen CPU temperature, voltage, current and power monitoring (DKMS)'
arch=('x86_64')
url='https://github.com/thor2002ro/zenpower'
license=('GPL-2.0-only')
depends=('dkms')
conflicts=('zenpower-dkms-git')
source=('Makefile' 'dkms.conf' 'zenpower.c' 'LICENSE')
sha256sums=('5a4a318f46be88bad8d0a133607a35db10b773920cac9459a74e6051273edcc9'
            'f2191696508cd1fbf6cd8a05ae4309cac60ca5511f1fbb23e02a1149adcc50d1'
            '6010adc20171dd2474ecd1dfee8986e720f4b44ff964f04a8d2548b70f815526'
            '8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643')

package() {
    local dkmsdir="$pkgdir/usr/src/zenpower-$pkgver"

    install -Dm644 "$srcdir/Makefile" "$dkmsdir/Makefile"
    install -Dm644 "$srcdir/zenpower.c" "$dkmsdir/zenpower.c"
    sed -e "s/@VERSION@/$pkgver/g" -e 's/@CFLGS@//g' \
        "$srcdir/dkms.conf" > "$dkmsdir/dkms.conf"
    chmod 644 "$dkmsdir/dkms.conf"
    install -Dm644 "$srcdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
