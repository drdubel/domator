from pickle import dump

with open("czupel/data/cookies.pickle", "wb") as cookies:
    dump({}, cookies)
