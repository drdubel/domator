import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';

/// Dark glassmorphism dashboard palette: deep-gray backgrounds, vibrant
/// gradient accents, translucent "glass" surfaces.
class AppColors {
  AppColors._();

  static const bgBase = Color(0xFF0B0F1A);
  static const bgSurface = Color(0xFF141B2E);
  static const bgSurfaceRaised = Color(0xFF1C2540);

  static const primary = Color(0xFF7C6CF6);
  static const primaryDeep = Color(0xFF5B4FE0);
  static const accentCyan = Color(0xFF22D3EE);
  static const accentPink = Color(0xFFEC4899);

  static const success = Color(0xFF34D399);
  static const danger = Color(0xFFF87171);
  static const warning = Color(0xFFFBBF24);

  static const textPrimary = Color(0xFFF4F6FB);
  static const textSecondary = Color(0xFFAEB6CE);
  static const textMuted = Color(0xFF6E7791);

  static const glassBg = Color(0x14FFFFFF);
  static const glassBorder = Color(0x22FFFFFF);
  static const glassHighlight = Color(0x33FFFFFF);

  // Ambient background gradient orbs (dark glassmorphism trend).
  static const orbPurple = Color(0xFF6D5DF6);
  static const orbBlue = Color(0xFF3B82F6);
  static const orbPink = Color(0xFFEC4899);

  // Blind motion-state glow colors.
  static const movingUpStart = Color(0xFF22D3EE);
  static const movingUpEnd = Color(0xFF0891B2);
  static const movingDownStart = Color(0xFFFB923C);
  static const movingDownEnd = Color(0xFFEA580C);
  static const stoppedStart = Color(0xFF34D399);
  static const stoppedEnd = Color(0xFF059669);
  static const inactiveOpacity = 0.35;

  static const upGradientStart = Color(0xFF7C6CF6);
  static const upGradientEnd = Color(0xFF5B4FE0);
  static const downGradientStart = Color(0xFF5B4FE0);
  static const downGradientEnd = Color(0xFF3730A3);
  static const stopGradientStart = Color(0xFF64748B);
  static const stopGradientEnd = Color(0xFF44506B);
}

ThemeData buildAppTheme() {
  final baseTextTheme = GoogleFonts.manropeTextTheme(ThemeData.dark().textTheme).apply(
    bodyColor: AppColors.textPrimary,
    displayColor: AppColors.textPrimary,
  );

  final colorScheme = ColorScheme.fromSeed(
    seedColor: AppColors.primary,
    brightness: Brightness.dark,
  ).copyWith(
    primary: AppColors.primary,
    secondary: AppColors.accentCyan,
    surface: AppColors.bgSurface,
    error: AppColors.danger,
    onSurface: AppColors.textPrimary,
  );

  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    colorScheme: colorScheme,
    scaffoldBackgroundColor: AppColors.bgBase,
    canvasColor: AppColors.bgBase,
    fontFamily: baseTextTheme.bodyMedium?.fontFamily,
    textTheme: baseTextTheme.copyWith(
      titleLarge: baseTextTheme.titleLarge?.copyWith(fontWeight: FontWeight.w800, letterSpacing: -0.3),
      titleMedium: baseTextTheme.titleMedium?.copyWith(fontWeight: FontWeight.w700),
      headlineMedium: baseTextTheme.headlineMedium?.copyWith(fontWeight: FontWeight.w800, letterSpacing: -0.5),
    ),
    appBarTheme: AppBarTheme(
      backgroundColor: Colors.transparent,
      foregroundColor: AppColors.textPrimary,
      elevation: 0,
      titleTextStyle: baseTextTheme.headlineSmall?.copyWith(fontWeight: FontWeight.w800),
    ),
    navigationBarTheme: NavigationBarThemeData(
      backgroundColor: AppColors.bgSurface.withValues(alpha: 0.9),
      elevation: 0,
      indicatorColor: AppColors.primary.withValues(alpha: 0.28),
      labelTextStyle: WidgetStateProperty.all(
        GoogleFonts.manrope(color: AppColors.textSecondary, fontSize: 12, fontWeight: FontWeight.w600),
      ),
      iconTheme: WidgetStateProperty.resolveWith(
        (states) => IconThemeData(
          color: states.contains(WidgetState.selected) ? AppColors.accentCyan : AppColors.textMuted,
        ),
      ),
    ),
    cardTheme: CardThemeData(
      color: AppColors.glassBg,
      surfaceTintColor: Colors.transparent,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(24),
        side: const BorderSide(color: AppColors.glassBorder),
      ),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: AppColors.glassBg,
      labelStyle: const TextStyle(color: AppColors.textMuted),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(14),
        borderSide: const BorderSide(color: AppColors.glassBorder, width: 1.5),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(14),
        borderSide: const BorderSide(color: AppColors.primary, width: 2),
      ),
    ),
    sliderTheme: SliderThemeData(
      activeTrackColor: AppColors.primary,
      inactiveTrackColor: AppColors.glassBg,
      thumbColor: AppColors.accentCyan,
      overlayColor: AppColors.primary.withValues(alpha: 0.2),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        backgroundColor: AppColors.primary,
        foregroundColor: Colors.white,
        padding: const EdgeInsets.symmetric(horizontal: 28, vertical: 16),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        textStyle: GoogleFonts.manrope(fontWeight: FontWeight.w700, fontSize: 15),
      ),
    ),
  );
}

/// Ambient gradient-orb backdrop behind the whole app, per the "dark
/// glassmorphism" trend: soft vibrant blurred color fields under glass
/// surfaces, rather than a flat background.
class AmbientBackground extends StatelessWidget {
  final Widget child;

  const AmbientBackground({super.key, required this.child});

  @override
  Widget build(BuildContext context) {
    return Stack(
      children: [
        Positioned.fill(
          child: DecoratedBox(
            decoration: const BoxDecoration(color: AppColors.bgBase),
            child: Stack(
              children: [
                _orb(top: -80, left: -60, color: AppColors.orbPurple, size: 260),
                _orb(top: 120, right: -100, color: AppColors.orbBlue, size: 300),
                _orb(bottom: -100, left: -40, color: AppColors.orbPink, size: 260),
              ],
            ),
          ),
        ),
        child,
      ],
    );
  }

  Widget _orb({double? top, double? bottom, double? left, double? right, required Color color, required double size}) {
    return Positioned(
      top: top,
      bottom: bottom,
      left: left,
      right: right,
      child: Container(
        width: size,
        height: size,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          gradient: RadialGradient(colors: [color.withValues(alpha: 0.28), color.withValues(alpha: 0.0)]),
        ),
      ),
    );
  }
}

/// Translucent "glass" container: blurred backdrop, subtle border, soft glow.
class GlassCard extends StatelessWidget {
  final Widget child;
  final EdgeInsetsGeometry padding;

  const GlassCard({super.key, required this.child, this.padding = const EdgeInsets.all(18)});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: padding,
      decoration: BoxDecoration(
        color: AppColors.glassBg,
        borderRadius: BorderRadius.circular(24),
        border: Border.all(color: AppColors.glassBorder),
        boxShadow: [
          BoxShadow(color: Colors.black.withValues(alpha: 0.25), blurRadius: 24, offset: const Offset(0, 8)),
        ],
      ),
      child: child,
    );
  }
}

/// Gradient action button matching the "up"/"down"/"stop" blind controls:
/// always shows its own gradient, dimmed (not grayed) when inactive.
enum ActionButtonKind { up, down, stop }

class GradientActionButton extends StatelessWidget {
  final ActionButtonKind kind;
  final IconData icon;
  final String label;
  final bool active;
  final VoidCallback onPressed;

  const GradientActionButton({
    super.key,
    required this.kind,
    required this.icon,
    required this.label,
    required this.active,
    required this.onPressed,
  });

  (Color, Color) get _inactiveGradient => switch (kind) {
        ActionButtonKind.up => (AppColors.upGradientStart, AppColors.upGradientEnd),
        ActionButtonKind.down => (AppColors.downGradientStart, AppColors.downGradientEnd),
        ActionButtonKind.stop => (AppColors.stopGradientStart, AppColors.stopGradientEnd),
      };

  (Color, Color) get _activeGradient => switch (kind) {
        ActionButtonKind.up => (AppColors.movingUpStart, AppColors.movingUpEnd),
        ActionButtonKind.down => (AppColors.movingDownStart, AppColors.movingDownEnd),
        ActionButtonKind.stop => (AppColors.stoppedStart, AppColors.stoppedEnd),
      };

  @override
  Widget build(BuildContext context) {
    final (start, end) = active ? _activeGradient : _inactiveGradient;

    return Opacity(
      opacity: active ? 1.0 : AppColors.inactiveOpacity,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          borderRadius: BorderRadius.circular(16),
          onTap: onPressed,
          child: Container(
            padding: const EdgeInsets.symmetric(vertical: 12, horizontal: 4),
            decoration: BoxDecoration(
              gradient: LinearGradient(
                begin: Alignment.topCenter,
                end: Alignment.bottomCenter,
                colors: [start, end],
              ),
              borderRadius: BorderRadius.circular(16),
              boxShadow: active ? [BoxShadow(color: end.withValues(alpha: 0.55), blurRadius: 18)] : null,
            ),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(icon, color: Colors.white, size: 20),
                const SizedBox(height: 4),
                Text(
                  label,
                  style: GoogleFonts.manrope(color: Colors.white, fontSize: 11, fontWeight: FontWeight.w700),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
