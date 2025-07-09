from pickle import dump

with open("czupel/cookies.pickle", "wb") as cookies:
    dump({}, cookies)
