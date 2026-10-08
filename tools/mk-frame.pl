#!/usr/bin/perl
# 生成 E87N 面板用的 428x142 RGB565 裸帧。
#
#   perl mk-frame.pl out.raw <模式>
#
# 模式：
#   orient   四角异色 + 左上半区黄块 + E87N 白字。判断朝向与镜像。
#   frame    1px 白边框 + 两条对角线。判断几何是否被剪切/错位。
#   topline  只在 y=0 画一条白线，y=141 画一条红线。判断行原点的位置。
#   leftline 只在 x=0 画一条白线，x=427 画一条红线。判断列原点的位置。
#   hbands   8 条等宽横色带。行寻址错位会立刻显形。
#   text     8 条竖色带 + 黑底白字 E87N
#   bars     8 条竖色带
#   gradient 双向渐变
#   checker  16px 棋盘格
#   gray / white / black   整屏纯色
#
# 帧格式：428 宽 x 142 高，每像素 16 位 RGB565，小端。整帧 428*142*2 = 121552 字节。
# 用 `screen-ctl raw out.raw` 或 `cat out.raw > /dev/fb0` 送到面板。
#
# 为什么需要 frame / topline / leftline 这三个几何探针：
# 驱动把 PASET 窗口写成 y+12（rotate=270 的固定偏移），若面板 GRAM 的该轴只有 142 长，
# 窗口就超出 12 行并回卷——纯色填充看不出回卷（同色绕回去还是同色），
# 只有结构化的图案才会显出剪切。这三个图案专门用来暴露这种错位。
use strict; use warnings;

my $out  = shift // die "用法: $0 out.raw <模式>\n";
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

sub fill_all { rect(0, 0, $W - 1, $H - 1, $_[0]) }

# 8x8 点阵字模，bit7 在最左
my %FONT = (
    'E' => [0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00],
    '8' => [0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00],
    '7' => [0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00],
    'N' => [0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00],
    ' ' => [0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00],
);

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
    rect(0,     0,     59,   29,   $C{red});
    rect($W-60, 0,     $W-1, 29,   $C{green});
    rect(0,     $H-30, 59,   $H-1, $C{blue});
    rect($W-60, $H-30, $W-1, $H-1, $C{white});
    rect(0, 44, 213, 55, $C{yellow});
    draw_text('E87N', 5, undef, 72, $C{white});

} elsif ($mode eq 'frame') {
    # 1px 边框 + 两条对角线。寻址整体错位时，边框会断开、对角线会折成两段。
    fill_all($C{black});
    for my $x (0 .. $W-1) { setpix($x, 0, $C{white}); setpix($x, $H-1, $C{white}) }
    for my $y (0 .. $H-1) { setpix(0, $y, $C{white}); setpix($W-1, $y, $C{white}) }
    for my $x (0 .. $W-1) {
        my $y1 = int($x * ($H - 1) / ($W - 1));
        setpix($x, $y1, $C{red});
        setpix($x, ($H - 1) - $y1, $C{green});
    }

} elsif ($mode eq 'topline') {
    # 只画两行：y=0 白、y=141 红。位置不对就说明行原点/跨度错。
    fill_all($C{black});
    for my $x (0 .. $W-1) { setpix($x, 0, $C{white}); setpix($x, $H-1, $C{red}) }

} elsif ($mode eq 'leftline') {
    fill_all($C{black});
    for my $y (0 .. $H-1) { setpix(0, $y, $C{white}); setpix($W-1, $y, $C{red}) }

} elsif ($mode eq 'hbands') {
    my @b = ($C{red}, $C{green}, $C{blue}, $C{white},
             $C{yellow}, $C{cyan}, $C{magenta}, $C{gray});
    for my $y (0 .. $H-1) {
        my $c = $b[int($y * scalar(@b) / $H)];
        for my $x (0 .. $W-1) { $px[$y*$W+$x] = $c }
    }

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

} elsif ($mode eq 'gray')  { fill_all($C{gray})  }
  elsif ($mode eq 'white') { fill_all($C{white}) }
  elsif ($mode eq 'black') { fill_all($C{black}) }
  else { die "未知模式: $mode\n" }

open my $fh, '>', $out or die "打不开 $out: $!";
binmode $fh;
my $buf = '';
$buf .= pack('v', $_) for @px;
print $fh $buf;
close $fh;

printf "写出 %-36s 模式=%-9s %d 字节  期望 %d  %s\n",
    $out, $mode, length($buf), $W * $H * 2,
    (length($buf) == $W * $H * 2 ? 'OK' : '不符!');
