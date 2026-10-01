"""Render the old and native pixel fonts in the actual UI layouts; no firmware writes."""
from pathlib import Path
import os
import runpy
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
generator = root / 'scripts/product_ui_layout.py'
previous = os.environ.get('ESTOP_UI_FONT')
try:
    os.environ['ESTOP_UI_FONT'] = 'system'
    old = runpy.run_path(str(generator))
    os.environ.pop('ESTOP_UI_FONT', None)
    new = runpy.run_path(str(generator))
finally:
    if previous is None:
        os.environ.pop('ESTOP_UI_FONT', None)
    else:
        os.environ['ESTOP_UI_FONT'] = previous

cases = [('菜单', 'menus', 0), ('短路提示', 'protection', 0),
         ('休眠提示', 'power', 3), ('未连接提示', 'connection', 2)]
scale = 4
preview = Image.new('RGB', (1084, 92 + len(cases) * 298), '#171b21')
draw = ImageDraw.Draw(preview)
heading = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 20)
draw.text((20, 10), '相同 128×64 布局，按原始像素放大 4 倍', font=heading, fill='white')
draw.text((20, 46), '原宋体 12 / 10 像素', font=heading, fill='white')
draw.text((560, 46), '缝合像素字体 12 / 10 像素', font=heading, fill='white')
for i, (label, group, index) in enumerate(cases):
    y = 92 + i * 298
    draw.text((20, y), label, font=heading, fill='white')
    for x, source in [(20, old), (560, new)]:
        native = source['GROUPS'][group][index][1]
        preview.paste(native.convert('RGB').resize((512, 256), Image.Resampling.NEAREST), (x, y + 30))
path = root / 'tmp/ui-previews/font-comparison.png'
path.parent.mkdir(parents=True, exist_ok=True)
preview.save(path)
print(path)
