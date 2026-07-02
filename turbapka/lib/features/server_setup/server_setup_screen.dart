import 'package:flutter/material.dart';
import 'package:google_fonts/google_fonts.dart';

import '../../core/config.dart';
import '../../core/theme.dart';

/// Asks the user for their turbacz backend's domain and persists it.
///
/// Shown on first launch before the login screen, since the app has no
/// backend baked in at build time.
class ServerSetupScreen extends StatefulWidget {
  final VoidCallback onConfigured;

  const ServerSetupScreen({super.key, required this.onConfigured});

  @override
  State<ServerSetupScreen> createState() => _ServerSetupScreenState();
}

class _ServerSetupScreenState extends State<ServerSetupScreen> {
  final _controller = TextEditingController(text: AppConfig.host ?? '');
  String? _error;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  Future<void> _save() async {
    final input = _controller.text.trim();
    if (input.isEmpty) {
      setState(() => _error = 'Enter your server address.');
      return;
    }

    await AppConfig.setHost(input);
    if (!mounted) return;
    widget.onConfigured();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.transparent,
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: GlassCard(
            padding: const EdgeInsets.all(36),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Container(
                  width: 72,
                  height: 72,
                  decoration: const BoxDecoration(
                    shape: BoxShape.circle,
                    gradient: LinearGradient(
                      begin: Alignment.topLeft,
                      end: Alignment.bottomRight,
                      colors: [AppColors.primary, AppColors.accentPink],
                    ),
                  ),
                  child: const Icon(Icons.dns_rounded, size: 36, color: Colors.white),
                ),
                const SizedBox(height: 20),
                Text('Connect to Turbacz', style: Theme.of(context).textTheme.headlineMedium),
                const SizedBox(height: 4),
                Text(
                  'Enter the address of your turbacz server',
                  textAlign: TextAlign.center,
                  style: GoogleFonts.manrope(color: AppColors.textSecondary, fontWeight: FontWeight.w500),
                ),
                const SizedBox(height: 24),
                TextField(
                  controller: _controller,
                  autocorrect: false,
                  keyboardType: TextInputType.url,
                  textInputAction: TextInputAction.done,
                  onSubmitted: (_) => _save(),
                  decoration: const InputDecoration(
                    labelText: 'Server address',
                    hintText: 'example.com',
                  ),
                ),
                if (_error != null) ...[
                  const SizedBox(height: 12),
                  Text(_error!, style: const TextStyle(color: AppColors.danger)),
                ],
                const SizedBox(height: 24),
                FilledButton.icon(
                  onPressed: _save,
                  icon: const Icon(Icons.arrow_forward),
                  label: const Text('Continue'),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
