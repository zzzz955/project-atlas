'use strict';
/**
 * project-atlas 경량 config 로더.
 * `template.ini` + `tools/types.json` 을 읽어 생성기 3종이 공유하는 설정 객체를 만든다.
 * 🔴 시크릿은 다루지 않는다 — 접속 정보 · 키는 .env 이고 이 파일은 경로/타깃만 본다.
 */
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

class ConfigError extends Error {
  constructor(source, message) {
    super(`[config] ERROR: ${source}: ${message}`);
    this.name = 'ConfigError';
  }
}

function rel(p) {
  return path.relative(ROOT, p).replaceAll(path.sep, '/');
}

function parseIni(content, sourcePath) {
  const cfg = {};
  let section = '_';
  for (const [index, raw] of content.split(/\r?\n/).entries()) {
    const line = raw.trim();
    if (!line || line.startsWith(';') || line.startsWith('#')) continue;
    const sec = line.match(/^\[(.+)\]$/);
    if (sec) {
      section = sec[1];
      cfg[section] = cfg[section] || {};
      continue;
    }
    const eq = line.indexOf('=');
    if (eq <= 0) throw new ConfigError(`${sourcePath}:${index + 1}`, `invalid ini line "${raw}"`);
    cfg[section] = cfg[section] || {};
    cfg[section][line.slice(0, eq).trim()] = line.slice(eq + 1).trim();
  }
  return cfg;
}

function requiredIni(ini, sourcePath, section, key) {
  const v = ini[section]?.[key];
  if (v !== undefined && v !== '') return v;
  throw new ConfigError(`${rel(sourcePath)} [${section}].${key}`, 'missing required value');
}

function requiredPath(ini, sourcePath, section, key) {
  return path.join(ROOT, requiredIni(ini, sourcePath, section, key));
}

function csv(value) {
  return value.split(',').map(s => s.trim()).filter(Boolean);
}

// 정규화 타입 → 엔진 타입 매핑. cpp 열이 이 레포의 seam이므로 필수로 검사한다.
function loadTypes() {
  const typesPath = path.join(ROOT, 'tools', 'types.json');
  if (!fs.existsSync(typesPath)) throw new ConfigError(rel(typesPath), 'file not found');
  let parsed;
  try {
    parsed = JSON.parse(fs.readFileSync(typesPath, 'utf-8'));
  } catch (e) {
    throw new ConfigError(rel(typesPath), `invalid JSON: ${e.message}`);
  }
  const types = {};
  for (const [key, value] of Object.entries(parsed)) {
    if (key.startsWith('_')) continue;
    if (!value || !value.cpp) {
      throw new ConfigError(`${rel(typesPath)} "${key}"`, 'each type must define "cpp"');
    }
    types[key] = value;
  }
  return types;
}

function load() {
  const iniPath = path.join(ROOT, 'template.ini');
  if (!fs.existsSync(iniPath)) throw new ConfigError(rel(iniPath), 'file not found');
  const ini = parseIni(fs.readFileSync(iniPath, 'utf-8'), rel(iniPath));

  return {
    root: ROOT,
    paths: {
      datasDir: requiredPath(ini, iniPath, 'paths', 'datas_dir'),
      serverGenerated: requiredPath(ini, iniPath, 'paths', 'server_generated'),
    },
    dataGen: {
      serverTargets: csv(requiredIni(ini, iniPath, 'data-gen', 'server_targets')),
      cppInfoOutputDir: requiredPath(ini, iniPath, 'data-gen', 'cpp_info_output_dir'),
      cppNamespace: requiredIni(ini, iniPath, 'data-gen', 'cpp_namespace'),
    },
    dbGen: {
      schemaPath: requiredPath(ini, iniPath, 'db-gen', 'schema'),
      sqlOutput: requiredPath(ini, iniPath, 'db-gen', 'sql_output'),
      cppDbOutputDir: requiredPath(ini, iniPath, 'db-gen', 'cpp_db_output_dir'),
      cppNamespace: requiredIni(ini, iniPath, 'db-gen', 'cpp_namespace'),
    },
    pktGen: {
      contractsDir: requiredPath(ini, iniPath, 'packet-gen', 'contracts_dir'),
      cppOutputDir: requiredPath(ini, iniPath, 'packet-gen', 'cpp_output_dir'),
      cppNamespace: requiredIni(ini, iniPath, 'packet-gen', 'cpp_namespace'),
    },
    types: loadTypes(),
  };
}

try {
  module.exports = load();
} catch (error) {
  console.error(error.message);
  process.exit(1);
}
