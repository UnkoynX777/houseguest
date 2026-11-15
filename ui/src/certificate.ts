// The policy stores a signer as the certificate subject exactly as Windows formats it, because that string is what
// rules compare. It is not meant to be read: it carries every attribute, some of them as bare OIDs. These helpers
// turn it into what people recognise and into a labelled list for the advanced view. They only change how the
// string is shown; the stored value is never altered.

export interface NameField {
  label: string;
  oid: string;
  value: string;
}

const known: Record<string, { oid: string; label: string }> = {
  CN: { oid: '2.5.4.3', label: 'Common name' },
  O: { oid: '2.5.4.10', label: 'Organization' },
  OU: { oid: '2.5.4.11', label: 'Organizational unit' },
  C: { oid: '2.5.4.6', label: 'Country' },
  S: { oid: '2.5.4.8', label: 'State or province' },
  ST: { oid: '2.5.4.8', label: 'State or province' },
  L: { oid: '2.5.4.7', label: 'Locality' },
  STREET: { oid: '2.5.4.9', label: 'Street' },
  POSTALCODE: { oid: '2.5.4.17', label: 'Postal code' },
  SERIALNUMBER: { oid: '2.5.4.5', label: 'Serial number' },
  T: { oid: '2.5.4.12', label: 'Title' },
  E: { oid: '1.2.840.113549.1.9.1', label: 'Email' },
  DC: { oid: '0.9.2342.19200300.100.1.25', label: 'Domain component' },
};

const byOid: Record<string, string> = {
  ...Object.fromEntries(Object.values(known).map((entry) => [entry.oid, entry.label])),
  '2.5.4.15': 'Business category',
  '2.5.4.97': 'Organization identifier',
  '1.3.6.1.4.1.311.60.2.1.1': 'Jurisdiction locality',
  '1.3.6.1.4.1.311.60.2.1.2': 'Jurisdiction state',
  '1.3.6.1.4.1.311.60.2.1.3': 'Jurisdiction country',
};

function split(text: string): string[] {
  const parts: string[] = [];
  let current = '';
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"') {
      if (quoted && text[i + 1] === '"') {
        current += '""';
        i++;
        continue;
      }
      quoted = !quoted;
      current += c;
    } else if ((c === ',' || c === ';') && !quoted) {
      parts.push(current);
      current = '';
    } else {
      current += c;
    }
  }
  if (current.trim()) parts.push(current);
  return parts;
}

function unquote(value: string): string {
  const trimmed = value.trim();
  if (trimmed.length >= 2 && trimmed.startsWith('"') && trimmed.endsWith('"')) return trimmed.slice(1, -1).replace(/""/g, '"');
  return trimmed;
}

export function parseSubject(subject: string): NameField[] {
  const fields: NameField[] = [];
  for (const part of split(subject.slice(0, 4096))) {
    const at = part.indexOf('=');
    if (at < 1) continue;
    let key = part.slice(0, at).trim();
    const value = unquote(part.slice(at + 1)).slice(0, 512);
    if (/^oid\./i.test(key)) key = key.slice(4);
    const short = known[key.toUpperCase()];
    const oid = short?.oid ?? (/^\d+(\.\d+)+$/.test(key) ? key : '');
    const label = short?.label ?? byOid[oid] ?? (oid ? `OID ${oid}` : key);
    fields.push({ label, oid, value });
    if (fields.length >= 32) break;
  }
  return fields;
}

export function signerName(subject: string): string {
  const fields = parseSubject(subject);
  return fields.find((f) => f.oid === '2.5.4.3')?.value || fields.find((f) => f.oid === '2.5.4.10')?.value || '';
}
