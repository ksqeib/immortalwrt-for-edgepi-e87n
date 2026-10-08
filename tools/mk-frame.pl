#!/usr/bin/perl
# 生成 E87N 面板用的 428x142 RGB565 裸帧。
#
#   perl mk-frame.pl out.raw [orient|text|bars|gradient|checker|gray|white|black]
#
# 帧格式：428 宽 x 142 高，每像素 16 位 RGB565，小端（ARM 默认字节序）。
# 整帧 428*142*2 = 121552 字节。用 `screen-ctl raw out.raw` 送到 /dev/fb0。
#
# orient 模式专用于判断面板朝向：fb 是横的 428x142，面板物理是竖的 142x428，
# 驱动靠 MADCTL 的 MV 位交换行列，所以画出来的东西在屏上可能转了 90 度或镜像。
# 四角颜色互不相同、黄块只占左上半区，看一眼就能确定映射关系。
use strict; use warnings;

my $out  = shift // die "用法: $0 out.raw [orient|text|bars|gradient|checker|gray|white|black]\n";
my $mode = shift // 'orient';

my ($W, $H) = (428, 142);
my @px = (0) x ($W * $H);

my %C = (red     => 0xF800, green => 0x07E0, blue  => 0x001F,
         white   => 0xFFFF, yellow => 0xFFE0, cyan => 0x07FF,
         magenta => 0xF81F, gray  => 0x8410, dark  => 0x2104,
         black   => 0x0000);

sub setpix {
    my ($x, $y, $c) = @_;
    return if $x < 0 || $x >= $W || $y < 0 || $y >= $H;
    $px[$y * $W + $x] = $c;
}

sub rect {
    my ($x0, $y0, $x1, $y1, $c) = @_;
    for my $y ($y0 .. $y1) { for my $x ($x0 .. $x1) { setpix($x, $y, $c) } }
}

sub fill_all {
    my ($c) = @_;
    rect(0, 0, $W - 1, $H - 1, $c);
}

# 8x8 点阵字模，bit7 在最左
my %FONT = (
    'E' => [0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00],
    '8' => [0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00],
    '7' => [0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00],
    'N' => [0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00],
    'G' => [0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00],
    'O' => [0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00],
    'K' => [0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00],
    ' ' => [0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00],
);

# 居中画一串字。$x0/$y0 传 undef 表示按该轴居中。
# $bg 非空时先铺一层底，保证压在任何背景上都看得清。
sub draw_text {
    my ($str, $scale, $x0, $y0, $fg, $bg) = @_;
    my @chars = split //, $str;
    my $cw = 8 * $scale;
    my $gap = 2 * $scale;
    my $tw = @chars * $cw + (@chars - 1) * $gap;
    my $th = 8 * $scale;
    my $bx0 = defined($x0) ? $x0 : int(($W - $tw) / 2);
    my $by0 = defined($y0) ? $y0 : int(($H - $th) / 2);
    if (defined $bg) {
        rect($bx0 - $scale, $by0 - $scale,
             $bx0 + $tw + $scale - 1, $by0 + $th + $scale - 1, $bg);
    }
    my $ci = 0;
    for my $ch (@chars) {
        my $g = $FONT{$ch};
        if (defined $g) {
            my $bx = $bx0 + $ci * ($cw + $gap);
            for my $ry (0 .. 7) {
                my $bits = $g->[$ry];
                for my $rx (0 .. 7) {
                    next unless $bits & (0x80 >> $rx);
                    for my $dy (0 .. $scale - 1) {
                        for my $dx (0 .. $scale - 1) {
                            setpix($bx + $rx * $scale + $dx,
                                   $by0 + $ry * $scale + $dy, $fg);
                        }
                    }
                }
            }
        }
        $ci++;
    }
}

my @BARS = ($C{red}, $C{green}, $C{blue}, $C{white},
            $C{yellow}, $C{cyan}, $C{magenta}, $C{gray});

if ($mode eq 'orient') {
    fill_all($C{dark});
    # 四角：红 绿 / 蓝 白，四个都不一样，看图就知道有没有镜像
    rect(0,     0,     59,   29,   $C{red});
    rect($W-60, 0,     $W-1, 29,   $C{green});
    rect(0,     $H-30, 59,   $H-1, $C{blue});
    rect($W-60, $H-30, $W-1, $H-1, $C{white});
    # 黄块只占左上半区：x 与 y 两个方向同时不对称，
    # 能区分「顺时针转 90」和「逆时针转 90」
    rect(0, 44, 213, 55, $C{yellow});
    # 文字放在下方居中，不与黄块重叠
    draw_text('E87N', 5, undef, 72, $C{white});
} elsif ($mode eq 'text') {
    for my $y (0 .. $H-1) {
        for my $x (0 .. $W-1) {
            $px[$y*$W+$x] = $BARS[int($x * scalar(@BARS) / $W)];
        }
    }
    draw_text('E87N', 8, undef, undef, $C{white}, $C{black});
} elsif ($mode eq 'bars') {
    for my $y (0 .. $H-1) {
        for my $x (0 .. $W-1) {
            $px[$y*$W+$x] = $BARS[int($x * scalar(@BARS) / $W)];
        }
    }
} elsif ($mode eq 'gradient') {
    for my $y (0 .. $H-1) {
        for my $x (0 .. $W-1) {
            my $r = int($x * 31 / ($W - 1));
            my $g = int($y * 63 / ($H - 1));
            my $b = int((($W - 1 - $x) * 31) / ($W - 1));
            $px[$y*$W+$x] = ($r << 11) | ($g << 5) | $b;
        }
    }
} elsif ($mode eq 'checker') {
    for my $y (0 .. $H-1) {
        for my $x (0 .. $W-1) {
            $px[$y*$W+$x] = ((int($x/16) + int($y/16)) % 2) ? $C{white} : $C{black};
        }
    }
} elsif ($mode eq 'gray') {
    fill_all($C{gray});
} elsif ($mode eq 'white') {
    fill_all($C{white});
} elsif ($mode eq 'black') {
    fill_all($C{black});
} else {
    die "未知模式: $mode\n";
}

open my $fh, '>', $out or die "打不开 $out: $!";
binmode $fh;
my $buf = '';
$buf .= pack('v', $_) for @px;
print $fh $buf;
close $fh;

printf "写出 %-34s 模式=%-9s %d 字节  期望 %d  %s\n",
    $out, $mode, length($buf), $W * $H * 2,
    (length($buf) == $W * $H * 2 ? 'OK' : '不符!');
