#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""云听（radio.cn）接口客户端 —— 直接复刻网页 api.js 的签名

签名规则（api.js，key 明文写在里面）：
    signText = "<把参数按键名排序后 a=1&b=2>" + "&timestamp=<ms>&key=f0fc4c668392f9f9a447e48584c214ee"
    sign     = MD5(signText).upper()
    headers  = { timestamp: ms, sign: sign, equipmentId: "0000", platformCode: "WEB",
                 Content-Type: "application/json" }
接口基址：https://ytmsout.radio.cn
"""
import os, ssl, time, json, hashlib, urllib.request, urllib.parse

KEY = 'f0fc4c668392f9f9a447e48584c214ee'
BASE = 'https://ytmsout.radio.cn'
UA = ('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 '
      '(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36')

CTX = ssl.create_default_context(); CTX.check_hostname = False; CTX.verify_mode = ssl.CERT_NONE
# 只放「有值」的代理：空字符串会让 ProxyHandler 把 host 当成 ''，
# 于是每次请求都抛 URLError: no host given（2026-11-04 踩到，fetch_xm2.py 早就是对的写法）
PH = {k: v for k, v in (('http', os.environ.get('http_proxy', '')),
                        ('https', os.environ.get('https_proxy', ''))) if v}
OP = urllib.request.build_opener(urllib.request.ProxyHandler(PH),
                                 urllib.request.HTTPSHandler(context=CTX))


def sign_of(params):
    s = '&'.join('%s=%s' % (k, params[k]) for k in sorted(params))
    tm = int(time.time() * 1000)
    txt = (s + '&' if s else '') + 'timestamp=%d&key=%s' % (tm, KEY)
    return tm, hashlib.md5(txt.encode('utf-8')).hexdigest().upper()


def call(path, params=None, method='get'):
    """path 形如 /web/appAlbum/detail"""
    params = dict(params or {})
    tm, sg = sign_of(params)
    H = {'User-Agent': UA, 'Accept': 'application/json',
         'equipmentId': '0000', 'platformCode': 'WEB',
         'timestamp': str(tm), 'sign': sg,
         'Content-Type': 'application/json',
         'Referer': 'https://www.radio.cn/'}
    url = BASE + path
    data = None
    if method.upper() == 'POST':
        data = json.dumps(params).encode()
    elif params:
        url += '?' + urllib.parse.urlencode(params)
    try:
        r = OP.open(urllib.request.Request(url, data=data, headers=H), timeout=25)
        txt = r.read().decode('utf-8', 'replace')
        return json.loads(txt)
    except Exception as e:
        return {'__err': '%s %s' % (type(e).__name__, str(e)[:80])}


if __name__ == '__main__':
    a = '16631244851730'
    print('--- album/detail ---')
    print(json.dumps(call('/web/appAlbum/detail', {'albumId': a}), ensure_ascii=False)[:600])
    print('--- pageByAlbum ---')
    print(json.dumps(call('/web/appSingle/pageByAlbum',
                          {'albumId': a, 'pageNo': 0, 'pageSize': 3}),
                     ensure_ascii=False)[:1200])
