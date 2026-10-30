#!/usr/bin/env python3
"""Audit public macro upstreams without importing or executing AKShare.

Only literal request construction is interpreted from the pinned source. Each
entry gets one bounded data-page probe; history pagination is intentionally not
run. Results describe host collection, not ESP32 support or source accuracy.
"""
import argparse
import ast
import concurrent.futures
import csv
import datetime as dt
import hashlib
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from zoneinfo import ZoneInfo

COMMIT = '62a56b2159add40f41b2c19129be3ac2af297d09'
NOW = dt.datetime.now(dt.timezone.utc)
TODAY = NOW.date()
STAMP = int(NOW.timestamp() * 1000)
MACRO_MODULES = ['macro_australia', 'macro_canada', 'macro_china', 'macro_china_hk',
                 'macro_china_money', 'macro_china_nbs', 'macro_cnbs',
                 'macro_constitute', 'macro_euro', 'macro_finance_ths',
                 'macro_germany', 'macro_info_ws', 'macro_japan',
                 'macro_news_baidu', 'macro_swiss', 'macro_uk', 'macro_usa']
EXTRA_MODULES = ['akshare/interest_rate/interest_rate_central_bank.py',
                 'akshare/interest_rate/interest_rate_interbank_em.py',
                 'akshare/interest_rate/interest_rate_repo.py',
                 'akshare/bond/bond_em.py']
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36'
JIN_HEADERS = {'User-Agent': UA, 'x-app-id': 'rU6QIu7JHe2gOUeR',
               'x-csrf-token': 'x-csrf-token', 'x-version': '1.0.0'}


class Unsupported(Exception):
    pass


def literal(n, env):
    """Small allowlist evaluator; never eval/exec downloaded code."""
    if isinstance(n, ast.Constant):
        return n.value
    if isinstance(n, ast.Name):
        return env[n.id]
    if isinstance(n, (ast.List, ast.Tuple)):
        return [literal(x, env) for x in n.elts]
    if isinstance(n, ast.Dict):
        return {literal(k, env): literal(v, env) for k, v in zip(n.keys, n.values)}
    if isinstance(n, ast.JoinedStr):
        return ''.join(str(literal(x.value, env)) if isinstance(x, ast.FormattedValue) else x.value for x in n.values)
    if isinstance(n, ast.BinOp):
        a, b = literal(n.left, env), literal(n.right, env)
        if isinstance(n.op, ast.Add):
            return a + b
        if isinstance(n.op, ast.Mult):
            return a * b
        if isinstance(n.op, ast.Div):
            return a / b
    if isinstance(n, ast.Subscript):
        return literal(n.value, env)[literal(n.slice, env)]
    if isinstance(n, ast.Slice):
        return slice(*(literal(x, env) if x else None for x in [n.lower, n.upper, n.step]))
    if isinstance(n, ast.Call):
        args = [literal(x, env) for x in n.args]
        if isinstance(n.func, ast.Name) and n.func.id in ('str', 'int', 'round', 'float'):
            return {'str': str, 'int': int, 'round': round, 'float': float}[n.func.id](*args)
        if isinstance(n.func, ast.Attribute):
            if isinstance(n.func.value, ast.Name) and n.func.value.id == 'time' and n.func.attr == 'time':
                return NOW.timestamp()
            obj = literal(n.func.value, env)
            if isinstance(obj, str) and n.func.attr in ('format', 'join', 'replace', 'strip'):
                return getattr(obj, n.func.attr)(*args)
    raise Unsupported(ast.dump(n)[:100])


def env_before(fn, stop, overrides=None):
    env = {'date': '', 't': NOW.timestamp()}
    defaults = [None] * (len(fn.args.args) - len(fn.args.defaults)) + fn.args.defaults
    for arg, default in zip(fn.args.args, defaults):
        if default is not None:
            try:
                env[arg.arg] = literal(default, env)
            except (Unsupported, KeyError, TypeError):
                pass
    env.update(overrides or {})
    # The historical examples in source are not valid defaults for live probes.
    for key in ('start_date', 'begin_date'):
        if key in env:
            env[key] = (TODAY - dt.timedelta(days=14)).strftime('%Y%m%d')
    if 'end_date' in env:
        env['end_date'] = TODAY.strftime('%Y%m%d')
    for n in sorted(ast.walk(fn), key=lambda x: getattr(x, 'lineno', 10**9)):
        if getattr(n, 'lineno', 10**9) >= stop:
            break
        if isinstance(n, ast.Assign):
            try:
                v = literal(n.value, env)
                for target in n.targets:
                    if isinstance(target, ast.Name):
                        env[target.id] = v
            except (Unsupported, KeyError, TypeError, ValueError, IndexError, ZeroDivisionError):
                pass
    return env


def request_calls(fn):
    return sorted([n for n in ast.walk(fn) if isinstance(n, ast.Call) and
                   isinstance(n.func, ast.Attribute) and n.func.attr in ('get', 'post') and
                   isinstance(n.func.value, ast.Name) and n.func.value.id in ('http', 'session')],
                  key=lambda n: n.lineno)


def extract(fn, functions, overrides=None, depth=0):
    if depth > 3:
        raise Unsupported('helper nesting')
    calls = request_calls(fn)
    # Euro helpers use a metadata /dates request before the actual data page.
    if fn.name.startswith('macro_euro_') and calls and '_lme_' not in fn.name:
        calls = calls[1:2]
    if calls:
        c = calls[0]
        env = env_before(fn, c.lineno, overrides)
        kw = {x.arg: x.value for x in c.keywords}
        url = literal(c.args[0] if c.args else kw['url'], env)
        params = literal(kw['params'], env) if 'params' in kw else {}
        body = literal(kw['data'], env) if 'data' in kw else None
        headers = literal(kw['headers'], env) if 'headers' in kw else {}
        return dict(url=url, params=params, body=body, method=c.func.attr.upper(), headers=headers)
    for c in sorted([n for n in ast.walk(fn) if isinstance(n, ast.Call) and isinstance(n.func, ast.Name) and n.func.id in functions], key=lambda n: n.lineno):
        helper = functions[c.func.id]
        env = env_before(fn, c.lineno, overrides)
        try:
            values = {a.arg: literal(v, env) for a, v in zip(helper.args.args, c.args)}
            values.update({k.arg: literal(k.value, env) for k in c.keywords})
            return extract(helper, functions, values, depth + 1)
        except (Unsupported, KeyError, ValueError, TypeError):
            pass
    raise Unsupported('No statically resolvable data request; requires a session, external helper, or file parser')


def budget(name):
    if name == 'macro_china_international_tourism_fx':
        return 730, 'annual; dates denote observation year'
    if name == 'macro_canada_gdp_monthly':
        return 140, 'monthly; allows normal two-month publication lag'
    if any(k in name for k in ('bank_rate', 'bank_', 'reserve_requirement')):
        return 190, 'event; older records require manual confirmation'
    if any(k in name for k in ('gdp', 'gbp', 'gbd', 'quarterly', 'enterprise_boom', 'hk_ppi')):
        return 190, 'quarterly'
    if any(k in name for k in ('cftc', 'rig_count', 'crude', 'jobless')):
        return 35, 'weekly'
    if any(k in name for k in ('shibor', 'market_info', 'daily_energy', 'china_rmb', 'market_margin',
                              'au_report', 'vegetable', 'agricultural', 'energy_index',
                              'commodity_price', 'sox', 'electronic', 'construction',
                              'bdti', 'bsi', 'shipping', 'lme', 'cons_gold', 'cons_silver',
                              'cme', 'interbank', 'repo_rate', 'swap_rate', 'bond_zh_us', 'bond_public')):
        return 14, 'daily'
    if name == 'macro_info_ws':
        return 2, 'calendar'
    return 95, 'monthly'


def parse_date(v):
    if not isinstance(v, str):
        return None
    s = v.strip()
    if re.fullmatch(r'\d{4}', s):
        return dt.date(int(s), 1, 1)
    m = re.match(r'^(\d{4})[-/.年]?(\d{1,2})(?:[-/.月]?(\d{1,2}))?', s)
    if m:
        try:
            return dt.date(int(m[1]), int(m[2]), int(m[3] or 1))
        except ValueError:
            return None
    q = re.match(r'^(\d{4}).*?([1-4])季度', s)
    if q:
        return dt.date(int(q[1]), int(q[2]) * 3, 1)
    return None


def has_number(value):
    if isinstance(value, bool) or value is None:
        return False
    if isinstance(value, (int, float)):
        return True
    if isinstance(value, str):
        return bool(re.fullmatch(r'[-+]?\d[\d,]*(?:\.\d+)?%?', value.strip()))
    if isinstance(value, list):
        return any(has_number(x) for x in value)
    if isinstance(value, dict):
        return any(has_number(v) for k, v in value.items() if not any(x in k.lower() for x in ('date', 'time', 'id', 'code', 'year')))
    return False


def observations(j, name):
    pairs = []
    if name == 'macro_cons_opec_month':
        data = j.get('data') or {}
        for column, key in enumerate(data.get('keys', [])):
            label = key.get('name', '')
            if re.fullmatch(r'\d{4}/\d{2}', label):
                d = parse_date(label)
                vals = [r[column] for r in data.get('values', []) if len(r) > column]
                if d and has_number(vals):
                    pairs.append((d, vals))
        return pairs
    if name == 'macro_info_ws':
        for row in j.get('data', {}).get('items', []):
            d = dt.datetime.fromtimestamp(row['public_date'], ZoneInfo('Asia/Shanghai')).date()
            if d == TODAY:
                pairs.append((d, row))  # Upcoming events need not have actuals.
        return pairs
    if isinstance(j, dict) and '_audit_rows' in j:
        rows = j['_audit_rows']
    elif isinstance(j, dict) and isinstance(j.get('result'), dict):
        rows = j['result'].get('data') or []
    elif isinstance(j, dict) and isinstance(j.get('data'), dict) and 'values' in j['data']:
        rows = j['data']['values']
    elif isinstance(j, dict) and 'values' in j:
        rows = j['values']
    elif isinstance(j, dict) and 'records' in j:
        rows = j['records']
    elif isinstance(j, dict) and 'data' in j:
        rows = j['data']
        if isinstance(rows, dict):
            rows = rows.get('非累计') or rows.get('累计') or rows
    elif isinstance(j, dict) and 'list' in j:
        rows = j['list']
    else:
        rows = []
    if isinstance(rows, dict):
        for key, val in rows.items():
            d = parse_date(key)
            if d and has_number(val):
                pairs.append((d, val))
    elif isinstance(rows, list):
        for row in rows:
            if isinstance(row, (list, tuple)) and row:
                d = parse_date(row[0])
                # Jin10 uses [date, actual, forecast, previous]. Forecasts are not actuals.
                val = row[1] if '/reports/list_v2' in str(j.get('_audit_url', '')) else row[1:]
                if d and has_number(val):
                    pairs.append((d, row))
            elif isinstance(row, dict):
                if isinstance(row.get('frValueMap'), dict):
                    row = row['frValueMap']
                date_keys = ['REPORT_DATE', 'TRADE_DATE', 'SOLAR_DATE', 'NOTICE_DATE', 'date', 'reportDate', 'showDate', 'releaseDate', 'issueDate', 'frDate']
                d = next((v for k in date_keys if (v := parse_date(row.get(k)))), None)
                if not d:
                    d = next((v for k, s in row.items() if ('date' in k.lower() or '日期' in k) and (v := parse_date(s))), None)
                val = row.get('VALUE') if 'VALUE' in row else row
                publication = parse_date(row.get('PUBLISH_DATE'))
                if d and has_number(val) and (not publication or publication <= TODAY):
                    pairs.append((d, row))
    return [(d, v) for d, v in pairs if d <= TODAY]


def prepare(entry, functions):
    name = entry['name']
    if name in ('macro_china_nbs_nation', 'macro_china_nbs_region', 'macro_china_urban_unemployment'):
        raise Unsupported('Statistics site source requires a warmed browser-compatible session; not admitted to anonymous firmware HTTP shortlist')
    if name == 'news_economic_baidu':
        raise Unsupported('External Baidu helper and optional cookie; no anonymous data probe completed')
    if name == 'macro_cnbs' or name.startswith('macro_rmb_') or name == 'macro_stock_finance' or name == 'macro_china_freight_index':
        raise Unsupported('Excel/HTML data scraper, outside JSON/CSV firmware API shortlist')
    if name == 'macro_info_ws':
        start = NOW.astimezone(ZoneInfo('Asia/Shanghai')).replace(hour=0, minute=0, second=0, microsecond=0)
        return dict(url='https://api-one-wscn.awtmt.com/apiv1/finance/macrodatas', method='GET', body=None,
                    params={'start': int(start.timestamp()), 'end': int((start + dt.timedelta(days=1)).timestamp())}, headers={})
    if name == 'macro_china_daily_energy':
        return dict(url='https://cdn.jin10.com/dc/reports/dc_qihuo_energy_report_all.js',
                    params={'v': STAMP, '_': STAMP + 90}, method='GET', body=None, headers={})
    if name == 'repo_rate_query':
        return dict(url='https://www.chinamoney.com.cn/r/cms/www/chinamoney/data/currency/frr-chrt.csv',
                    params={}, method='GET', body=None, headers={})
    request = extract(functions[name], functions)
    for key in ('params', 'body'):
        params = request[key]
        if isinstance(params, dict):
            for size in ('pageSize', 'ps', 'num'):
                if size in params:
                    params[size] = '60' if size != 'num' else '31'
            if 'max_date' in params:
                params['max_date'] = ''
    if name == 'macro_china_swap_rate':
        request['body'].update(startDate=(TODAY - dt.timedelta(days=14)).isoformat(), endDate=TODAY.isoformat())
    # These are data lookup POSTs, not state-changing operations.
    if request['method'] == 'POST' and not any(s in request['url'] for s in ('shrzgmQuery', '/IfccHis', '/bnBondEmit', '/FrrHis')):
        raise Unsupported('POST not in verified read-only lookup allowlist')
    return request


def unquoted_json_keys(s):
    """Quote bare object keys outside string literals; never execute JSONP."""
    out, index, quote, escaped = [], 0, None, False
    while index < len(s):
        c = s[index]
        if quote:
            out.append(c)
            if escaped:
                escaped = False
            elif c == '\\':
                escaped = True
            elif c == quote:
                quote = None
            index += 1
        elif c == "'":
            # Sina uses single-quoted object keys. Parse only this string token.
            end, single_escaped = index + 1, False
            while end < len(s):
                if not single_escaped and s[end] == "'":
                    end += 1
                    break
                if not single_escaped and s[end] == '\\':
                    single_escaped = True
                else:
                    single_escaped = False
                end += 1
            out.append(json.dumps(ast.literal_eval(s[index:end]), ensure_ascii=False))
            index = end
        elif c == '"':
            quote = c
            out.append(c)
            index += 1
        elif c.isalpha() or c in '_$':
            end = index + 1
            while end < len(s) and (s[end].isalnum() or s[end] in '_$'):
                end += 1
            after = end
            while after < len(s) and s[after].isspace():
                after += 1
            word = s[index:end]
            out.append(json.dumps(word) if after < len(s) and s[after] == ':' else word)
            index = end
        else:
            out.append(c)
            index += 1
    return ''.join(out)


def decode(raw, url):
    try:
        s = raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        s = raw.decode('gb18030')
    if url.endswith('.csv'):
        return {'_audit_rows': list(csv.reader(s.splitlines()))}
    try:
        j = json.loads(s)
    except json.JSONDecodeError:
        # JSONP or a JSON object assigned to a JS variable.
        j = json.loads(unquoted_json_keys(s[s.index('{'):s.rindex('}') + 1]))
    return {'_audit_rows': j} if isinstance(j, list) else j


def probe(entry, request, raw_dir):
    result = {**entry, 'request': request}
    result['maximum_age_days'], result['frequency_rule'] = budget(entry['name'])
    start = time.monotonic()
    try:
        params = urllib.parse.urlencode(request['params'])
        url = request['url'] + (('&' if '?' in request['url'] else '?') + params if params else '')
        body = urllib.parse.urlencode(request['body']).encode() if request['body'] is not None else None
        headers = {'User-Agent': UA, 'Accept': 'application/json', **request['headers']}
        headers = {k: v for k, v in headers.items() if k.lower() not in ('accept-encoding', 'content-length', 'host', 'connection')}
        req = urllib.request.Request(url, data=body, headers=headers, method=request['method'])
        with urllib.request.urlopen(req, timeout=12) as response:
            raw = response.read(4 * 1024 * 1024 + 1)
            result.update(http_status=response.status, response_bytes=len(raw))
        if len(raw) > 4 * 1024 * 1024:
            raise Unsupported('Response exceeds 4 MiB host audit cap')
        digest = hashlib.sha256(raw).hexdigest()
        result['sha256'] = digest
        (raw_dir / (entry['name'] + '.body')).write_bytes(raw)
        j = decode(raw, request['url'])
        if not isinstance(j, dict):
            raise Unsupported('Unsupported root data shape')
        if entry['name'] == 'macro_cons_opec_month':
            dates = j.get('data') or []
            valid_dates = [(parse_date(x), x) for x in dates if parse_date(x)]
            if not valid_dates:
                raise Unsupported('No report dates in OPEC metadata')
            _, report_date = max(valid_dates)
            data_url = 'https://datacenter-api.jin10.com/reports/list?' + urllib.parse.urlencode({'category': 'opec', 'date': report_date, '_': STAMP})
            with urllib.request.urlopen(urllib.request.Request(data_url, headers=headers), timeout=12) as response:
                raw = response.read(4 * 1024 * 1024 + 1)
                result['data_http_status'] = response.status
            if len(raw) > 4 * 1024 * 1024:
                raise Unsupported('OPEC response exceeds audit cap')
            (raw_dir / (entry['name'] + '-data.body')).write_bytes(raw)
            result.update(data_request_url=data_url, data_sha256=hashlib.sha256(raw).hexdigest(), data_response_bytes=len(raw))
            j = decode(raw, data_url)
        j['_audit_url'] = request['url']
        pairs = observations(j, entry['name'])
        result['valid_observations'] = len(pairs)
        if not pairs:
            result.update(status='unverified', reason='No dated numeric actuals parsed; inspect response/schema before admission')
        else:
            date, row = max(pairs, key=lambda x: x[0])
            result.update(latest_actual=date.isoformat(), age_days=(TODAY - date).days,
                          latest_sample=row)
            if result['age_days'] > result['maximum_age_days']:
                result.update(status='stale', reason='Last numeric actual exceeds freshness budget; removed from active shortlist')
            else:
                result.update(status='available', reason='Anonymous host HTTP, parsed numeric actual and freshness checks passed')
        if j.get('success') is False:
            result.update(status='failed', reason=str(j.get('message') or 'Upstream success=false'))
        if isinstance(j.get('head'), dict) and j['head'].get('rep_code') not in (None, '', '0', '0000', '200'):
            result.update(status='failed', reason='Upstream business error: ' + str(j['head'].get('rep_message') or j['head']['rep_code']))
    except urllib.error.HTTPError as e:
        result.update(status='failed', http_status=e.code, reason=f'HTTP {e.code}')
    except (OSError, ValueError, KeyError, TypeError, Unsupported) as e:
        result.update(status='unverified', reason=f'{type(e).__name__}: {str(e)[:180]}')
    result['elapsed_seconds'] = round(time.monotonic() - start, 3)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Pinned AKShare source directory; missing files download from fixed GitHub commit')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--raw', type=Path, required=True)
    parser.add_argument('--only', nargs='+', help='Recheck these names and merge into existing report')
    args = parser.parse_args()
    args.source.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.raw.mkdir(parents=True, exist_ok=True)
    sources = [f'akshare/macro/{x}.py' for x in MACRO_MODULES] + EXTRA_MODULES
    entries = []
    for file in sources:
        path = args.source / file
        if not path.exists():
            raw = urllib.request.urlopen(f'https://raw.githubusercontent.com/akfamily/akshare/{COMMIT}/{file}', timeout=20).read()
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
        functions = {n.name: n for n in ast.parse(path.read_text()).body if isinstance(n, ast.FunctionDef)}
        for name, fn in functions.items():
            if name.startswith('_') or name.endswith('_core'):
                continue
            if args.only and name not in args.only:
                continue
            doc = ast.get_docstring(fn) or ''
            description = doc.splitlines()[0] if doc else name
            entry = dict(name=name, source_file=file, source_line=fn.lineno, description=description)
            try:
                request = prepare(entry, functions)
                entries.append((entry, request))
            except (Unsupported, KeyError, TypeError, ValueError) as e:
                entries.append(({**entry, 'status': 'unverified', 'reason': str(e)[:250]}, None))
    results = []
    print(f'Audit scope: {len(entries)} entries; {sum(r is not None for _, r in entries)} resolved requests', flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures = {pool.submit(probe, e, r, args.raw): e for e, r in entries if r is not None}
        results.extend(e for e, r in entries if r is None)
        for future in concurrent.futures.as_completed(futures):
            r = future.result()
            results.append(r)
            print(json.dumps({k: r.get(k) for k in ('name', 'status', 'http_status', 'latest_actual', 'reason')}, ensure_ascii=False), flush=True)
    results.sort(key=lambda x: (x['source_file'], x['source_line']))
    if args.only and args.output.exists():
        previous = json.loads(args.output.read_text())['results']
        changed = {r['name'] for r in results}
        results += [r for r in previous if r['name'] not in changed]
        results.sort(key=lambda x: (x['source_file'], x['source_line']))
    report = dict(source_commit=COMMIT, tested_utc=NOW.isoformat(), scope='AKShare macro directory excluding five core helpers, plus central-bank/interbank/repo rates and China-US yields',
                  credentials='Anonymous requests, only public constants from source; no personal cookies or paid keys',
                  akshare_executed=False, esp32_tested=False,
                  definition='available = host response has parsed dated numeric actuals within per-entry freshness budget; not independent verification of statistical definitions or all historical rows',
                  results=results)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    counts = {s: sum(r['status'] == s for r in results) for s in ('available', 'stale', 'failed', 'unverified')}
    print(json.dumps(dict(total=len(results), counts=counts, output=str(args.output)), ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
