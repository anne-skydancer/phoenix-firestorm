//! Memory-safe parser for the SL binary-LLSD format, mirroring the C++
//! `LLSDBinaryParser` (indra/llcommon/llsdserialize.cpp) exactly.
//!
//! Grammar (all integer sizes/ints are big-endian / network byte order):
//!   '!' undef | '0' false | '1' true
//!   'i' + i32be | 'r' + f64be | 'u' + 16 bytes (uuid) | 'd' + f64be (date)
//!   's' + u32be len + bytes | 'l' + u32be len + bytes (uri)
//!   'b' + u32be len + bytes (binary)
//!   '[' + u32be count + values + ']'
//!   '{' + u32be count + (('k' + string) | '\'' | '"') key + value pairs + '}'
//!
//! Every read is bounds-checked against the input slice: malformed asset bytes
//! yield `None`, never an out-of-bounds read. This is the whole point of moving
//! the untrusted parse into Rust.

/// Max recursion depth (matches UNZIP_LLSD_MAX_DEPTH intent: bound the stack).
pub const MAX_DEPTH: i32 = 32;

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Undef,
    Bool(bool),
    Int(i32),
    Real(f64),
    Uuid([u8; 16]),
    /// 's' string or 'l' uri; raw bytes (mesh keys/strings are ASCII).
    Str(Vec<u8>),
    /// 'b' binary blob (the quantized vertex/index arrays live here).
    Binary(Vec<u8>),
    Array(Vec<Value>),
    /// Order-preserving key/value pairs (small maps; linear lookup is fine).
    Map(Vec<(Vec<u8>, Value)>),
}

impl Value {
    /// Look up a map key by ASCII name. Returns `None` for non-maps / missing keys.
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Map(entries) => entries
                .iter()
                .find(|(k, _)| k.as_slice() == key.as_bytes())
                .map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_binary(&self) -> Option<&[u8]> {
        match self {
            Value::Binary(b) => Some(b),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(a) => Some(a),
            _ => None,
        }
    }

    pub fn as_real(&self) -> Option<f64> {
        match self {
            Value::Real(r) => Some(*r),
            Value::Int(i) => Some(*i as f64),
            _ => None,
        }
    }

    pub fn has(&self, key: &str) -> bool {
        self.get(key).is_some()
    }
}

/// Bounds-checked forward cursor over the input bytes.
struct Cursor<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Cursor { buf, pos: 0 }
    }

    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        let end = self.pos.checked_add(n)?;
        let s = self.buf.get(self.pos..end)?;
        self.pos = end;
        Some(s)
    }

    fn u8(&mut self) -> Option<u8> {
        self.take(1).map(|s| s[0])
    }

    fn u32_be(&mut self) -> Option<u32> {
        let s = self.take(4)?;
        Some(u32::from_be_bytes([s[0], s[1], s[2], s[3]]))
    }

    /// LLSD sizes are signed i32; negative lengths are rejected (as C++ does).
    fn size_be(&mut self) -> Option<usize> {
        let n = self.u32_be()? as i32;
        if n < 0 {
            None
        } else {
            Some(n as usize)
        }
    }

    fn f64_be(&mut self) -> Option<f64> {
        let s = self.take(8)?;
        Some(f64::from_be_bytes([
            s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
        ]))
    }
}

/// Parse one binary-LLSD document from `buf`, after any deprecated header has
/// been stripped. Returns the root value, or `None` on malformed input.
pub fn parse(buf: &[u8]) -> Option<Value> {
    let mut c = Cursor::new(strip_header(buf));
    parse_value(&mut c, MAX_DEPTH)
}

/// Mirror of C++ `strip_deprecated_header`: skip a literal "<? LLSD/Binary ?>"
/// (17 bytes) if present. Also tolerate a modern "<?llsd/binary?>\n"-style line
/// (skip through the first newline) if the block leads with '<'.
fn strip_header(buf: &[u8]) -> &[u8] {
    const DEPRECATED: &[u8] = b"<? LLSD/Binary ?>";
    if buf.len() > DEPRECATED.len() && &buf[..DEPRECATED.len()] == DEPRECATED {
        let rest = &buf[DEPRECATED.len()..];
        // strip a single trailing newline if present
        return rest.strip_prefix(b"\n").unwrap_or(rest);
    }
    if buf.first() == Some(&b'<') {
        if let Some(nl) = buf.iter().position(|&b| b == b'\n') {
            return &buf[nl + 1..];
        }
    }
    buf
}

fn parse_value(c: &mut Cursor, depth: i32) -> Option<Value> {
    if depth <= 0 {
        return None;
    }
    let t = c.u8()?;
    match t {
        b'!' => Some(Value::Undef),
        b'0' => Some(Value::Bool(false)),
        b'1' => Some(Value::Bool(true)),
        b'i' => {
            let s = c.take(4)?;
            Some(Value::Int(i32::from_be_bytes([s[0], s[1], s[2], s[3]])))
        }
        b'r' => Some(Value::Real(c.f64_be()?)),
        b'd' => Some(Value::Real(c.f64_be()?)), // date: 8-byte f64, treat as real
        b'u' => {
            let s = c.take(16)?;
            let mut id = [0u8; 16];
            id.copy_from_slice(s);
            Some(Value::Uuid(id))
        }
        b's' | b'l' => {
            let n = c.size_be()?;
            Some(Value::Str(c.take(n)?.to_vec()))
        }
        b'b' => {
            let n = c.size_be()?;
            Some(Value::Binary(c.take(n)?.to_vec()))
        }
        b'[' => parse_array(c, depth),
        b'{' => parse_map(c, depth),
        _ => None,
    }
}

fn parse_array(c: &mut Cursor, depth: i32) -> Option<Value> {
    let size = c.size_be()?;
    let mut out: Vec<Value> = Vec::with_capacity(size.min(1 << 16));
    let mut count = 0usize;
    loop {
        // peek for ']'
        let next = *c.buf.get(c.pos)?;
        if next == b']' {
            break;
        }
        if count >= size {
            break;
        }
        out.push(parse_value(c, depth - 1)?);
        count += 1;
    }
    if c.u8()? != b']' || count < size {
        return None;
    }
    Some(Value::Array(out))
}

fn parse_map(c: &mut Cursor, depth: i32) -> Option<Value> {
    let size = c.size_be()?;
    let mut out: Vec<(Vec<u8>, Value)> = Vec::with_capacity(size.min(1 << 16));
    let mut count = 0usize;
    loop {
        let marker = c.u8()?;
        if marker == b'}' {
            break;
        }
        if count >= size {
            // consumed a non-'}' marker past the declared size -> malformed
            return None;
        }
        let key = match marker {
            b'k' => {
                let n = c.size_be()?;
                c.take(n)?.to_vec()
            }
            // Quoted (notation-style) keys are not used by mesh data; reject
            // rather than guess the delimiter-escape rules.
            _ => return None,
        };
        let val = parse_value(c, depth - 1)?;
        out.push((key, val));
        count += 1;
    }
    if count < size {
        return None;
    }
    Some(Value::Map(out))
}

#[cfg(test)]
mod tests {
    use super::*;

    // Hand-built binary LLSD: array[ map{ "i": i32(7), "b": binary[3] } ]
    #[test]
    fn parse_array_map_binary() {
        let mut b = Vec::new();
        b.push(b'[');
        b.extend_from_slice(&1u32.to_be_bytes()); // array size 1
        b.push(b'{');
        b.extend_from_slice(&2u32.to_be_bytes()); // map size 2
        b.push(b'k');
        b.extend_from_slice(&1u32.to_be_bytes());
        b.push(b'i'); // key "i"
        b.push(b'i');
        b.extend_from_slice(&7i32.to_be_bytes()); // value int 7
        b.push(b'k');
        b.extend_from_slice(&1u32.to_be_bytes());
        b.push(b'b'); // key "b"
        b.push(b'b');
        b.extend_from_slice(&3u32.to_be_bytes());
        b.extend_from_slice(&[9, 8, 7]); // value binary
        b.push(b'}');
        b.push(b']');

        let v = parse(&b).expect("parse ok");
        let arr = v.as_array().unwrap();
        assert_eq!(arr.len(), 1);
        assert_eq!(arr[0].get("i"), Some(&Value::Int(7)));
        assert_eq!(arr[0].get("b").unwrap().as_binary(), Some(&[9u8, 8, 7][..]));
    }

    #[test]
    fn truncated_is_none_not_panic() {
        // 'b' claims 100 bytes but only 2 follow -> must be None, never OOB.
        let mut b = vec![b'b'];
        b.extend_from_slice(&100u32.to_be_bytes());
        b.extend_from_slice(&[1, 2]);
        assert_eq!(parse(&b), None);
    }
}
