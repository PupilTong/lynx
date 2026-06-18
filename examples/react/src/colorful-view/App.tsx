/**
 * Copyright 2025 The Lynx Authors. All rights reserved.
 * Licensed under the Apache License Version 2.0 that can be found in the
 * LICENSE file in the root directory of this source tree.
 */

import './style.css';

export function App() {
  const colors = {
    level1: ['77', '00', 'ff'],
    level2: [
      '00',
      '11',
      '22',
      '33',
      '44',
      '55',
      '66',
      '77',
      '88',
      '99',
      'aa',
      'bb',
      'cc',
      'dd',
      'ee',
      'ff',
    ],
    level3: [
      '00',
      '11',
      '22',
      '33',
      '44',
      '55',
      '66',
      '77',
      '88',
      '99',
      'aa',
      'bb',
      'cc',
      'dd',
      'ee',
      'ff',
    ],
    level4: ['00', '11', '22', '33', '44', '55', '66', '77'],
  };

  return (
    <view className="root">
      {colors.level1.map((color1) => (
        <view
          className="outer"
          style={{ backgroundColor: '#' + color1 + color1 + color1 }}
        >
          {colors.level2.map((color2) => (
            <view
              className="block1"
              style={{ backgroundColor: '#' + color1 + color2 + color2 }}
            >
              {colors.level3.map((color3) => (
                <view
                  className="block2"
                  style={{ backgroundColor: '#' + color1 + color2 + color3 }}
                >
                  {colors.level4.map((color4) => (
                    <view
                      className="block3"
                      style={{
                        backgroundColor: '#' + color2 + color3 + color4,
                      }}
                    ></view>
                  ))}
                </view>
              ))}
            </view>
          ))}
        </view>
      ))}
    </view>
  );
}
