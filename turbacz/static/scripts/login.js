// ============================================
// LOGIN PAGE - Turbacz OAuth
// ============================================

document.addEventListener('DOMContentLoaded', function () {
    const googleSignInButton = document.getElementById('googleSignIn');

    // Handle Google Sign In button click
    googleSignInButton.addEventListener('click', function (e) {
        // Show loading state
        this.classList.add('loading');
        this.querySelector('span').textContent = 'Redirecting...';

        // Optional: Add analytics or logging here
        console.log('Redirecting to Google OAuth...');
    });


    // Add hover effect sound (optional)
    googleSignInButton.addEventListener('mouseenter', function () {
        this.style.transform = 'translateY(-2px) scale(1.02)';
    });

    googleSignInButton.addEventListener('mouseleave', function () {
        if (!this.classList.contains('loading')) {
            this.style.transform = '';
        }
    });

    // Handle page visibility change (pause animations when tab is hidden)
    document.addEventListener('visibilitychange', function () {
        const orbs = document.querySelectorAll('.gradient-orb');
        if (document.hidden) {
            orbs.forEach(orb => orb.style.animationPlayState = 'paused');
        } else {
            orbs.forEach(orb => orb.style.animationPlayState = 'running');
        }
    });

    // Add keyboard shortcut (Enter key to login)
    document.addEventListener('keypress', function (e) {
        if (e.key === 'Enter' && !googleSignInButton.classList.contains('loading')) {
            googleSignInButton.click();
        }
    });

    // Prevent double-click spam
    let isRedirecting = false;
    googleSignInButton.addEventListener('click', function (e) {
        if (isRedirecting) {
            e.preventDefault();
            return false;
        }
        isRedirecting = true;
    });
});

// Optional: Create floating particles effect
function createParticles() {
    const particlesContainer = document.createElement('div');
    particlesContainer.className = 'particles';
    particlesContainer.style.cssText = `
		position: fixed;
		top: 0;
		left: 0;
		width: 100%;
		height: 100%;
		pointer-events: none;
		z-index: 0;
		overflow: hidden;
	`;

    for (let i = 0; i < 250; i++) {
        const particle = document.createElement('div');
        const size = Math.random() * 3 + 1;
        const duration = Math.random() * 20 + 15;
        const delay = Math.random() * 5;
        const startX = Math.random() * 100;

        particle.style.cssText = `
			position: absolute;
			width: ${size}px;
			height: ${size}px;
			background: rgba(102, 126, 234, ${Math.random() * 0.5 + 0.2});
			border-radius: 50%;
			left: ${startX}%;
			bottom: -10px;
			animation: particleFloat ${duration}s linear infinite;
			animation-delay: ${delay}s;
			box-shadow: 0 0 ${size * 2}px rgba(102, 126, 234, 0.5);
		`;
        particlesContainer.appendChild(particle);
    }

    document.body.insertBefore(particlesContainer, document.body.firstChild);
}

// Add CSS for particle animation
const style = document.createElement('style');
style.textContent = `
	@keyframes particleFloat {
		0% {
			transform: translateY(0) translateX(0) scale(0);
			opacity: 0;
		}
		10% {
			opacity: 1;
			transform: translateY(-10vh) translateX(${Math.random() * 40 - 20}px) scale(1);
		}
		90% {
			opacity: 1;
		}
		100% {
			transform: translateY(-110vh) translateX(${Math.random() * 80 - 40}px) scale(0.5);
			opacity: 0;
		}
	}

	/* Smooth entrance animation */
	.login-box {
		animation: fadeInScale 0.6s cubic-bezier(0.34, 1.56, 0.64, 1);
	}

	@keyframes fadeInScale {
		0% {
			opacity: 0;
			transform: scale(0.9);
		}
		100% {
			opacity: 1;
			transform: scale(1);
		}
	}

	/* Google button ripple effect */
	.google-signin-button {
		position: relative;
		overflow: hidden;
	}

	.google-signin-button::after {
		content: '';
		position: absolute;
		top: 50%;
		left: 50%;
		width: 0;
		height: 0;
		border-radius: 50%;
		background: rgba(66, 133, 244, 0.3);
		transform: translate(-50%, -50%);
		transition: width 0.6s, height 0.6s;
	}

	.google-signin-button:active::after {
		width: 300px;
		height: 300px;
	}
`;
document.head.appendChild(style);

// Uncomment to enable floating particles
createParticles();