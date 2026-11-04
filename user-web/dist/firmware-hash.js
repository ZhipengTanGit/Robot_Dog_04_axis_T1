/** MD5 is used only for the ESP ROM's flash readback protocol, not authentication. */
export function md5(input) {
  if (!(input instanceof Uint8Array)) throw new TypeError('MD5 input must be bytes');
  const length = Math.ceil((input.length + 9) / 64) * 64;
  const bytes = new Uint8Array(length); bytes.set(input); bytes[input.length] = 0x80;
  const view = new DataView(bytes.buffer), bits = input.length * 8;
  view.setUint32(length - 8, bits >>> 0, true); view.setUint32(length - 4, Math.floor(bits / 4294967296), true);
  const h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
  const shifts = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  for (let offset = 0; offset < length; offset += 64) {
    let [a, b, c, d] = h;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
      else { f = c ^ (b | ~d); g = (7 * i) % 16; }
      const k = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);
      const sum = (a + f + k + view.getUint32(offset + g * 4, true)) >>> 0;
      const shift = shifts[Math.floor(i / 16) * 4 + i % 4];
      const next = (b + ((sum << shift) | (sum >>> (32 - shift)))) >>> 0;
      a = d; d = c; c = b; b = next;
    }
    [a, b, c, d].forEach((value, i) => { h[i] = (h[i] + value) >>> 0; });
  }
  return h.map(value => [0, 8, 16, 24].map(shift => ((value >>> shift) & 255).toString(16).padStart(2, '0')).join('')).join('');
}

export async function sha256(bytes) {
  const hash = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(hash)].map(byte => byte.toString(16).padStart(2, '0')).join('');
}
