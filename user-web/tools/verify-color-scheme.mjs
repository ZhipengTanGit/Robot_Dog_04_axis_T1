import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

// Exercise the production validators in an isolated realm. Storage and UI below
// are in-memory stubs; the test never accesses browser data or writes files.
const source = fs.readFileSync(new URL('../dist/app.js', import.meta.url), 'utf8').replace(/^import[^\n]+\n/, '');
const definitions = source.slice(0, source.indexOf('const motionPreference')) +
  source.slice(source.indexOf('function expandLegacyColors('), source.indexOf('function buildRegionButtons(')) +
  source.slice(source.indexOf('function validFaceId('), source.indexOf('function onViewerSelect(')) +
  source.slice(source.indexOf('function plainObject('), source.indexOf('function renderSaved('));

const tests = `
let checks = 0;
const check = (actual, expected) => { assert.deepEqual(actual, expected); checks += 1; };
const throws = (run) => { assert.throws(run); checks += 1; };
const clone = value => JSON.parse(JSON.stringify(value));
const structuredClone = clone;
const motionPreference = {matches:false};
function renderSaved() {}
function renderControls() {}
function syncAppearance() {}
function showToast() {}
let storageRaw = null;
let storageWrites = 0;
const localStorage = {
  getItem() { return storageRaw; },
  setItem(key, value) { assert.equal(key, STORAGE_KEY); storageRaw = value; storageWrites += 1; },
};

const base = {format:'robot-color-studio', version:2, name:'button test', material:'satin', backdrop:'light', view:'iso', autoRotate:false};
const parentFace = '打印件_身体_上盖-1/f29/shell';
const childFace = parentFace + '~posts';
const buttonFace = '打印件_身体_电源按钮-1/f7/accent';
const oldTwelve = {...base,
  colors:Object.fromEntries(Object.keys(GROUPS).filter(key => key !== 'powerButton').map(key => [key, '#A59E99'])),
  groupOverrides:{accent:'#aabbcc'}, faceOverrides:{[buttonFace]:'#abcdef', [parentFace]:'#112233'},
};
oldTwelve.colors.accent = '#aabbcc';
const oldSnapshot = JSON.stringify(oldTwelve);
const migrated = validateScheme(oldTwelve);
check(migrated.colors.powerButton, '#AABBCC');
check(migrated.groupOverrides.powerButton, '#AABBCC');
check(migrated.faceOverrides[buttonFace], '#ABCDEF');
check(JSON.stringify(oldTwelve), oldSnapshot);
check(Object.keys(migrated.colors).length, 13);
check(validateScheme(migrated), migrated);
const oldEleven = clone(oldTwelve); delete oldEleven.colors.posts;
check(validateScheme(oldEleven).groupOverrides.powerButton, '#AABBCC');
check(validateScheme(oldEleven).colors.posts, originalColors.posts);
const oldNative = clone(oldTwelve); oldNative.groupOverrides = {};
check(Object.hasOwn(validateScheme(oldNative).groupOverrides, 'powerButton'), false);
const five = {...base, colors:{shell:'#010203',legs:'#112233',frameAccent:'#223344',legAccent:'#334455',accent:'#445566'},groupOverrides:{accent:'#445566'},faceOverrides:{[buttonFace]:'#abcdef'}};
check(validateScheme(five).colors.powerButton, '#445566');
check(validateScheme(five).groupOverrides.powerButton, '#445566');
const three = {...base, version:1, colors:{shell:'#010203',legs:'#112233',accent:'#445566'}};
check(validateScheme(three).groupOverrides.powerButton, '#445566');
const independent = {...migrated, colors:{...migrated.colors, powerButton:'#123456', accent:'#654321'},groupOverrides:{powerButton:'#123456',accent:'#654321'}};
check(validateScheme(independent).groupOverrides, {powerButton:'#123456',accent:'#654321'});
const topOnly = {...independent, groupOverrides:{accent:'#654321'}};
check(Object.hasOwn(validateScheme(topOnly).groupOverrides, 'powerButton'), false);
state.colors = {...independent.colors}; state.groupOverrides = {...independent.groupOverrides}; state.faceOverrides = {...independent.faceOverrides};
check(validateScheme(currentScheme()).groupOverrides, {powerButton:'#123456',accent:'#654321'});
for (const preset of PRESETS) {
  const palette = expandLegacyColors(preset.colors);
  check(palette.powerButton, preset.colors.powerButton);
  check(palette.accent, preset.colors.accent);
}
const malformed = clone(independent); delete malformed.colors.shell;
throws(() => validateScheme(malformed));
const unknown = clone(independent); unknown.colors.pcb = '#000000';
throws(() => validateScheme(unknown));
const badColor = clone(oldTwelve); badColor.colors.accent = 'red';
throws(() => validateScheme(badColor));

// Initial local loading precedes viewer metadata. Keep provenance in memory,
// and retain the original format if another save rewrites the list meanwhile.
storageRaw = JSON.stringify([{id:'old-scheme',createdAt:'2026-10-29T00:00:00.000Z',scheme:oldTwelve}]);
loadSavedSchemes();
check(pendingLegacyFaceSources.has(savedSchemes[0].scheme), true);
check(Object.hasOwn(savedSchemes[0].scheme.faceOverrides, childFace), false);
check(storageWrites, 0);
persistSaved(savedSchemes);
check(Object.hasOwn(JSON.parse(storageRaw)[0].scheme.colors, 'powerButton'), false);
check(JSON.parse(storageRaw)[0].scheme.faceOverrides[parentFace], '#112233');
applyScheme(savedSchemes[0].scheme, 'test');
check(activeLegacyFacesPending, true);
throws(() => currentScheme());
adoptFaceColorParents({faceColorParents:[{faceId:childFace,parentFaceId:parentFace}]});
check(savedSchemes[0].scheme.faceOverrides[childFace], '#112233');
check(savedSchemes[0].scheme.faceOverrides[parentFace], '#112233');
check(pendingLegacyFaceSources.has(savedSchemes[0].scheme), false);
check(state.faceOverrides[childFace], '#112233');
check(activeLegacyFacesPending, false);
check(storageWrites, 1);
persistSaved(savedSchemes);
check(Object.hasOwn(JSON.parse(storageRaw)[0].scheme.colors, 'powerButton'), true);
check(JSON.parse(storageRaw)[0].scheme.faceOverrides[childFace], '#112233');

// A fresh legacy import inherits once; explicit child colors always win.
const readyLegacy = validateScheme(oldTwelve);
check(readyLegacy.faceOverrides[childFace], '#112233');
check(readyLegacy.faceOverrides[parentFace], '#112233');
const explicit = clone(oldTwelve); explicit.faceOverrides[childFace] = '#fedcba';
check(validateScheme(explicit).faceOverrides[childFace], '#FEDCBA');
const fiveWithParent = clone(five); fiveWithParent.faceOverrides[parentFace] = '#123abc';
check(validateScheme(fiveWithParent).faceOverrides[childFace], '#123ABC');

// A 13-region document is an intentional new scheme. Clearing a split face
// must remain cleared through export/import, even while the parent is colored.
delete state.faceOverrides[childFace];
const cleared = validateScheme(currentScheme());
check(Object.hasOwn(cleared.faceOverrides, childFace), false);
check(cleared.faceOverrides[parentFace], '#112233');
const modern = clone(independent); modern.faceOverrides = {[parentFace]:'#a1b2c3'};
check(Object.hasOwn(validateScheme(modern).faceOverrides, childFace), false);
check(pendingLegacyFaceSources.has(validateScheme(modern)), false);
const modernExplicit = clone(modern); modernExplicit.faceOverrides[childFace] = '#987654';
check(validateScheme(modernExplicit).faceOverrides[childFace], '#987654');
check(Object.keys(readyLegacy).sort(), ['format','version','name','colors','groupOverrides','faceOverrides','material','backdrop','view','autoRotate'].sort());
throws(() => validateScheme({...modern, faceColorParents:[]}));
throws(() => adoptFaceColorParents({faceColorParents:[{faceId:childFace,parentFaceId:parentFace},{faceId:childFace,parentFaceId:parentFace}]}));
throws(() => adoptFaceColorParents({faceColorParents:[{faceId:'x'.repeat(257),parentFaceId:parentFace}]}));
throws(() => adoptFaceColorParents({faceColorParents:[{faceId:parentFace,parentFaceId:parentFace}]}));
console.log('Color scheme migration: ' + checks + ' checks passed');
`;

vm.runInNewContext(definitions + tests, {assert, console});
