import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseSubject, signerName } from '../../ui/src/certificate.ts';

const riot =
  'CN="Riot Games, Inc.", O="Riot Games, Inc.", L=Los Angeles, S=California, C=US, SERIALNUMBER=4558474, ' +
  '2.5.4.15=Private Organization, OID.1.3.6.1.4.1.311.60.2.1.2=Delaware, 1.3.6.1.4.1.311.60.2.1.3=US';

test('the signer is the common name, with quoted commas kept', () => {
  assert.equal(signerName(riot), 'Riot Games, Inc.');
});

test('attributes without a short name get a label, and the OID stays available', () => {
  const fields = parseSubject(riot);
  const jurisdiction = fields.find((f) => f.oid === '1.3.6.1.4.1.311.60.2.1.3');
  assert.equal(jurisdiction?.label, 'Jurisdiction country');
  assert.equal(jurisdiction?.value, 'US');
  assert.equal(fields.find((f) => f.oid === '2.5.4.15')?.label, 'Business category');
  assert.equal(fields.length, 9);
});

test('the organization is used when there is no common name', () => {
  assert.equal(signerName('O=Example Ltd, C=GB'), 'Example Ltd');
});

test('an unknown OID is labelled by its number and never becomes the signer', () => {
  const fields = parseSubject('1.2.3.4.5=mystery');
  assert.equal(fields[0].label, 'OID 1.2.3.4.5');
  assert.equal(signerName('1.2.3.4.5=mystery'), '');
});

test('doubled quotes inside a quoted value are unescaped', () => {
  assert.equal(signerName('CN="The ""Best"" Co, Inc."'), 'The "Best" Co, Inc.');
});

test('empty, malformed and oversized input is handled', () => {
  assert.deepEqual(parseSubject(''), []);
  assert.equal(signerName('not a subject'), '');
  assert.ok(parseSubject('CN=' + 'x'.repeat(100000))[0].value.length <= 512);
  assert.ok(parseSubject(Array.from({ length: 200 }, (_, i) => `OU=u${i}`).join(', ')).length <= 32);
});
